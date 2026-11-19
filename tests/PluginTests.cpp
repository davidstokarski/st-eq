// Whole-plugin tests (response per processing mode, latency, dynamics, stress, state, CPU) and editor snapshots.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#if JUCE_MAC
 #include <AudioToolbox/AudioToolbox.h>
#endif

using namespace steq;

namespace
{
int failures = 0, checks = 0;
void check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok) { ++failures; std::cout << "FAIL: " << what << "\n"; }
}

double toDb (double m) { return 20.0 * std::log10 (std::max (1.0e-12, m)); }

void setParam (StEqProcessor& p, const juce::String& id, float plain)
{
    auto& prm = p.param (id);
    prm.setValueNotifyingHost (prm.convertTo0to1 (plain));
}

void clearBands (StEqProcessor& p)
{
    for (int b = 0; b < kMaxBands; ++b) p.removeBand (b);
}

void setMode (StEqProcessor& p, ProcMode m, int res = 1)
{
    setParam (p, ids::mode, (float) (int) m);
    setParam (p, ids::resolution, (float) res);
    p.applyModeChange();
}

/** Impulse response of the processor (channel 0 and 1), `len` samples. */
std::array<std::vector<float>, 2> impulse (StEqProcessor& p, int len, int block = 512, bool rightImpulse = false)
{
    p.refreshFirNow();
    p.getEngine().reset();
    std::array<std::vector<float>, 2> out { std::vector<float> ((size_t) len), std::vector<float> ((size_t) len) };
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    // settle parameter smoothing / designs with silence first
    for (int i = 0; i < 8; ++i) { buf.clear(); p.processBlock (buf, midi); }
    p.getEngine().reset();
    for (int pos = 0; pos < len; pos += block)
    {
        buf.clear();
        if (pos == 0) buf.setSample (rightImpulse ? 1 : 0, 0, 1.0f);
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block && pos + i < len; ++i) out[(size_t) c][(size_t) (pos + i)] = buf.getSample (c, i);
    }
    return out;
}

/** Magnitude (dB) of an impulse response at hz via direct DFT. */
double magAt (const std::vector<float>& h, double hz, double fs)
{
    std::complex<double> s;
    const double w = juce::MathConstants<double>::twoPi * hz / fs;
    for (size_t n = 0; n < h.size(); ++n) s += (double) h[n] * std::polar (1.0, -w * (double) n);
    return toDb (std::abs (s));
}

double analogDb (StEqProcessor& p, double hz, Placement channelOf = Placement::stereo)
{
    double db = 0.0;
    const float scale = p.refs.readGainScale();
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto bp = p.getBand (b);
        if (! bp.isActive()) continue;
        if (bp.place != Placement::stereo && bp.place != channelOf) continue;
        db += st::dsp::eq::designAnalog (makeSpec (bp, scale)).magnitudeDb (hz);
    }
    return db;
}

float runNoise (StEqProcessor& p, int blocks, int blockSize, bool& finite, float amp = 0.25f)
{
    juce::AudioBuffer<float> buf (2, blockSize);
    juce::Random rng (1);
    juce::MidiBuffer midi;
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < blockSize; ++i)
                buf.setSample (c, i, (rng.nextFloat() * 2.0f - 1.0f) * amp + 0.3f * std::sin ((float) (b * blockSize + i) * 0.05f));
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < blockSize; ++i)
            {
                const float v = buf.getSample (c, i);
                if (! std::isfinite (v)) finite = false;
                peak = juce::jmax (peak, std::abs (v));
            }
    }
    return peak;
}

/** Steady-state output level (dB) of a sine at hz with input level inDb. */
double sineGainDb (StEqProcessor& p, double hz, double inDb, double fs = 48000.0, int seconds = 2)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    const float a = (float) juce::Decibels::decibelsToGain (inDb);
    double phase = 0.0, peak = 0.0;
    const int total = (int) (seconds * fs);
    for (int pos = 0; pos < total; pos += 512)
    {
        for (int i = 0; i < 512; ++i)
        {
            const float v = a * (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * hz / fs;
            buf.setSample (0, i, v); buf.setSample (1, i, v);
        }
        p.processBlock (buf, midi);
        if (pos > total - (int) (0.25 * fs))
            for (int i = 0; i < 512; ++i) peak = std::max (peak, (double) std::abs (buf.getSample (0, i)));
    }
    return toDb (peak) - inDb;
}

/** Output level (dB re input) of two simultaneous sines, measured by DFT over the last half second. */
std::pair<double, double> twoToneGainDb (StEqProcessor& p, double f1, double db1, double f2, double db2, double fs = 48000.0, int seconds = 3)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    const double a1 = juce::Decibels::decibelsToGain (db1), a2 = juce::Decibels::decibelsToGain (db2);
    const int total = (int) (seconds * fs), measureFrom = total - (int) (0.5 * fs);
    std::complex<double> s1, s2;
    int count = 0;
    for (int pos = 0; pos < total; pos += 512)
    {
        for (int i = 0; i < 512; ++i)
        {
            const double t = (pos + i) / fs;
            const float v = (float) (a1 * std::sin (juce::MathConstants<double>::twoPi * f1 * t) + a2 * std::sin (juce::MathConstants<double>::twoPi * f2 * t));
            buf.setSample (0, i, v); buf.setSample (1, i, v);
        }
        p.processBlock (buf, midi);
        for (int i = 0; i < 512 && pos >= measureFrom; ++i)
        {
            const double t = (pos + i) / fs, v = buf.getSample (0, i);
            s1 += v * std::polar (1.0, -juce::MathConstants<double>::twoPi * f1 * t);
            s2 += v * std::polar (1.0, -juce::MathConstants<double>::twoPi * f2 * t);
            ++count;
        }
    }
    return { toDb (2.0 * std::abs (s1) / count) - db1, toDb (2.0 * std::abs (s2) / count) - db2 };
}
} // namespace

//==============================================================================
static void testResponses (StEqProcessor& p)
{
    std::cout << "Responses per processing mode\n";
    const double fs = 48000.0;
    clearBands (p);
    p.addBand (Shape::lowCut, 40.0f, 0.0f, 0.71f, 3);
    p.addBand (Shape::bell, 250.0f, -6.0f, 1.5f);
    p.addBand (Shape::bell, 3000.0f, 5.0f, 0.8f);
    p.addBand (Shape::highShelf, 9000.0f, 4.0f, 0.71f);
    p.addBand (Shape::bell, 16000.0f, 6.0f, 0.7f);

    const double freqs[] = { 30, 60, 120, 250, 500, 1000, 2000, 3000, 5000, 8000, 12000, 15000, 17000, 19000 };

    struct M { ProcMode mode; const char* name; double tol; double tolHigh; };
    for (auto m : { M { ProcMode::zeroLatency, "Zero Latency", 0.75, 1.0 },
                    M { ProcMode::naturalPhase, "Natural Phase", 0.3, 0.6 },
                    M { ProcMode::linearPhase, "Linear Phase", 0.3, 0.3 } })
    {
        setMode (p, m.mode, 2);   // linear phase: High resolution
        const int lat = p.getLatencySamples();
        const int len = juce::nextPowerOfTwo (lat + 16384);
        auto h = impulse (p, len);
        double worst = 0.0;
        for (double f : freqs)
        {
            const double err = std::abs (magAt (h[0], f, fs) - analogDb (p, f));
            const double tol = f > 14000.0 ? m.tolHigh : m.tol;
            worst = std::max (worst, err);
            check (err < tol, juce::String (m.name) + " response at " + juce::String (f) + " Hz err " + juce::String (err, 3) + " dB");
        }
        // latency: the impulse response's energy is centred at the reported latency (linear phase) /
        // starts at it (natural phase: minimum-phase-like response after the FIR delay)
        int peakIdx = 0;
        for (int i = 0; i < len; ++i) if (std::abs (h[0][(size_t) i]) > std::abs (h[0][(size_t) peakIdx])) peakIdx = i;
        double pre = 0.0, total = 0.0;
        for (int i = 0; i < len; ++i) total += (double) h[0][(size_t) i] * h[0][(size_t) i];
        for (int i = 0; i < juce::jmax (0, lat - 8); ++i) pre += (double) h[0][(size_t) i] * h[0][(size_t) i];
        pre /= juce::jmax (1.0e-12, total);
        if (m.mode == ProcMode::linearPhase)
            check (std::abs (peakIdx - lat) <= 1, "linear phase peak at latency (" + juce::String (peakIdx) + " vs " + juce::String (lat) + ")");
        else if (m.mode == ProcMode::naturalPhase)
            check (pre < 1.0e-3, "natural phase: nothing before latency (" + juce::String (pre, 6) + ")");
        else
            check (lat == 0 && peakIdx < 4, "zero latency");
        // linear phase: symmetric impulse response around the latency
        if (m.mode == ProcMode::linearPhase)
        {
            double asym = 0.0, energy = 0.0;
            for (int i = 1; i < 2000; ++i)
            {
                asym += std::abs (h[0][(size_t) (lat + i)] - h[0][(size_t) (lat - i)]);
                energy += std::abs (h[0][(size_t) (lat + i)]);
            }
            check (asym < 1.0e-3 * juce::jmax (1.0, energy), "linear phase symmetric (" + juce::String (asym, 6) + ")");
        }
        std::cout << "  " << m.name << ": latency " << lat << ", worst |measured - analog| " << juce::String (worst, 3) << " dB\n";
    }

    // Mid/side + left/right placement in linear phase uses the 2x2 FIR matrix
    clearBands (p);
    p.addBand (Shape::bell, 1000.0f, 6.0f, 1.0f, 1, Placement::mid);
    p.addBand (Shape::bell, 4000.0f, -6.0f, 1.0f, 1, Placement::side);
    p.addBand (Shape::bell, 300.0f, 4.0f, 1.0f, 1, Placement::left);
    for (auto mode : { ProcMode::zeroLatency, ProcMode::linearPhase })
    {
        setMode (p, mode, 0);
        const int lat = p.getLatencySamples();
        auto hl = impulse (p, juce::nextPowerOfTwo (lat + 16384), 512, false);
        auto hr = impulse (p, juce::nextPowerOfTwo (lat + 16384), 512, true);
        // mid = (L+R)/2 input -> mid output: (hl[0] + hr[0] + hl[1] + hr[1]) / 2 ... check mid gain at 1 kHz on a mid signal
        std::vector<float> midOut (hl[0].size());
        std::vector<float> sideOut (hl[0].size());
        for (size_t i = 0; i < midOut.size(); ++i)
        {
            // input L=R=1 (mid): out L = hl0 + hr0, out R = hl1 + hr1 ; mid out = average
            midOut[i] = 0.5f * (hl[0][i] + hr[0][i] + hl[1][i] + hr[1][i]);
            sideOut[i] = 0.5f * ((hl[0][i] - hr[0][i]) - (hl[1][i] - hr[1][i]));
        }
        const double midDb = magAt (midOut, 1000.0, fs);
        const double sideDb = magAt (sideOut, 4000.0, fs);
        check (std::abs (midDb - 6.0) < 0.6, juce::String ((int) mode) + " mid band +6 at 1k on mid (" + juce::String (midDb, 2) + ")");
        check (std::abs (sideDb + 6.0) < 0.6, juce::String ((int) mode) + " side band -6 at 4k on side (" + juce::String (sideDb, 2) + ")");
    }
    setMode (p, ProcMode::zeroLatency);
}

static void testDynamics (StEqProcessor& p)
{
    std::cout << "Dynamic EQ\n";
    clearBands (p);
    const int b = p.addBand (Shape::bell, 1000.0f, 0.0f, 1.0f);
    setParam (p, ids::band (b, "dyn_range"), -9.0f);
    setParam (p, ids::band (b, "dyn_auto"), 0.0f);
    setParam (p, ids::band (b, "dyn_thresh"), -30.0f);

    for (auto mode : { ProcMode::zeroLatency, ProcMode::linearPhase })
    {
        setMode (p, mode, 0);
        p.refreshFirNow();
        const double loud = sineGainDb (p, 1000.0, -6.0);
        const double quiet = sineGainDb (p, 1000.0, -50.0);
        const double off = sineGainDb (p, 200.0, -6.0);
        check (loud < -6.0 && loud > -9.5, juce::String ((int) mode) + " loud sine compressed (" + juce::String (loud, 2) + " dB)");
        check (std::abs (quiet) < 0.3, juce::String ((int) mode) + " quiet sine untouched (" + juce::String (quiet, 2) + " dB)");
        check (std::abs (off) < 1.5, juce::String ((int) mode) + " off-band loud sine nearly untouched (" + juce::String (off, 2) + " dB)");
        std::cout << "  mode " << (int) mode << ": loud " << juce::String (loud, 2) << " dB, quiet " << juce::String (quiet, 2) << " dB, off-band " << juce::String (off, 2) << " dB\n";
    }
    setMode (p, ProcMode::zeroLatency);
}

static void testSpectral (StEqProcessor& p)
{
    std::cout << "Spectral dynamics\n";
    clearBands (p);
    setMode (p, ProcMode::zeroLatency);
    const int b = p.addBand (Shape::bell, 2000.0f, 0.0f, 0.5f);
    setParam (p, ids::band (b, "dyn_range"), -12.0f);
    setParam (p, ids::band (b, "dyn_auto"), 0.0f);
    setParam (p, ids::band (b, "dyn_thresh"), -30.0f);

    // Regular dynamic band: a loud tone pulls the whole band down, the quiet neighbour too.
    auto regular = twoToneGainDb (p, 2500.0, -6.0, 1400.0, -50.0);

    setParam (p, ids::band (b, "spectral"), 1.0f);
    p.applyModeChange();
    check (p.getEngine().isSpectralActive() && p.getLatencySamples() == 2048, "spectral stage adds its FFT size of latency (" + juce::String (p.getLatencySamples()) + ")");
    auto spectral = twoToneGainDb (p, 2500.0, -6.0, 1400.0, -50.0);
    std::cout << "  regular: loud " << juce::String (regular.first, 2) << " dB, quiet neighbour " << juce::String (regular.second, 2)
              << " dB | spectral: loud " << juce::String (spectral.first, 2) << " dB, quiet neighbour " << juce::String (spectral.second, 2) << " dB\n";
    check (spectral.first < -5.0, "spectral: loud tone compressed (" + juce::String (spectral.first, 2) + ")");
    check (std::abs (spectral.second) < 1.0, "spectral: quiet tone in the same band untouched (" + juce::String (spectral.second, 2) + ")");
    check (regular.second < -4.0, "regular dynamics pull the quiet tone down too (" + juce::String (regular.second, 2) + ")");

    // Below threshold: transparent (output = input delayed by the latency).
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        juce::Random rng (9);
        std::vector<float> in, out;
        p.getEngine().reset();
        for (int blk = 0; blk < 40; ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = (rng.nextFloat() * 2.0f - 1.0f) * 0.003f; buf.setSample (0, i, v); buf.setSample (1, i, v); in.push_back (v); }
            p.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) out.push_back (buf.getSample (0, i));
        }
        const int lat = p.getLatencySamples();
        double err = 0.0;
        for (size_t i = (size_t) lat + 4096; i < out.size(); ++i) err = std::max (err, (double) std::abs (out[i] - in[i - (size_t) lat]));
        check (err < 2.0e-5, "spectral: transparent below threshold (" + juce::String (err, 8) + ")");
    }

    // Linear phase + spectral: latencies add up.
    setMode (p, ProcMode::linearPhase, 1);
    check (p.getLatencySamples() == p.getEngine().getPhaseConfig().latency + 2048, "linear phase + spectral latency");
    p.refreshFirNow();
    auto lp = twoToneGainDb (p, 2500.0, -6.0, 1400.0, -50.0);
    check (lp.first < -5.0 && std::abs (lp.second) < 1.0, "linear phase spectral (" + juce::String (lp.first, 2) + " / " + juce::String (lp.second, 2) + ")");

    // Switching spectral off removes the stage and its latency.
    setParam (p, ids::band (b, "spectral"), 0.0f);
    setMode (p, ProcMode::zeroLatency);
    check (! p.getEngine().isSpectralActive() && p.getLatencySamples() == 0, "spectral off: no latency");
    clearBands (p);
}

static void testStress (StEqProcessor& p)
{
    std::cout << "Stress\n";
    bool finite = true;
    juce::Random rng (3);
    for (int mode = 0; mode < 3; ++mode)
    {
        setMode (p, (ProcMode) mode, mode == 2 ? 0 : 1);
        for (int round = 0; round < 6; ++round)
        {
            clearBands (p);
            for (int b = 0; b < kMaxBands; ++b)
            {
                const auto shape = (Shape) ((b + round) % 10);
                const int slot = p.addBand (shape, 20.0f * std::pow (1000.0f, rng.nextFloat()), rng.nextFloat() * 36.0f - 18.0f,
                                            0.1f * std::pow (100.0f, rng.nextFloat()), (b * 3 + round) % 10, (Placement) ((b + round) % 5));
                if (b % 3 == 0) setParam (p, ids::band (slot, "dyn_range"), rng.nextFloat() * 24.0f - 12.0f);
                if (b % 5 == 0) setParam (p, ids::band (slot, "gq"), 1.0f);
            }
            setParam (p, ids::character, (float) (round % 3));
            setParam (p, ids::gainScale, round % 2 ? 150.0f : 60.0f);
            p.refreshFirNow();
            const float peak = runNoise (p, 30, 512, finite, 0.2f);
            check (peak < 200.0f, "bounded mode " + juce::String (mode) + " round " + juce::String (round) + " peak " + juce::String (peak));
        }
        check (p.getLatencySamples() == p.getEngine().getLatencySamples(), "latency reported mode " + juce::String (mode));
    }
    check (finite, "all output finite");
    setParam (p, ids::character, 0.0f);
    setParam (p, ids::gainScale, 100.0f);

    // odd block sizes + sample rates
    for (double sr : { 44100.0, 96000.0 })
        for (int bs : { 1, 7, 63, 64, 65, 333, 2048 })
        {
            p.prepareToPlay (sr, bs);
            const float peak = runNoise (p, 8, bs, finite);
            check (finite && peak < 200.0f, "block size " + juce::String (bs) + " @" + juce::String (sr));
        }
    p.prepareToPlay (48000.0, 512);

    // solo
    clearBands (p);
    p.addBand (Shape::bell, 1000.0f, 3.0f, 1.0f);
    p.soloBand = 0;
    const double atBand = sineGainDb (p, 1000.0, -12.0, 48000.0, 1);
    const double offBand = sineGainDb (p, 100.0, -12.0, 48000.0, 1);
    p.soloBand = -1;
    check (std::abs (atBand) < 1.0 && offBand < -12.0, "solo passes the band region only (" + juce::String (atBand, 1) + " / " + juce::String (offBand, 1) + ")");
}

static void testState (StEqProcessor& p)
{
    std::cout << "State / band management\n";
    clearBands (p);
    const int a = p.addBand (Shape::bell, 500.0f, 4.0f, 2.0f);
    const int b = p.addBand (Shape::highCut, 9000.0f, 0.0f, 0.71f, 3);
    check (a == 0 && b == 1 && p.getNumUsedBands() == 2, "addBand fills slots");
    p.removeBand (a);
    check (p.getNumUsedBands() == 1 && ! p.getBand (0).used, "removeBand");
    const int c = p.addBand (Shape::notch, 60.0f, 0.0f, 10.0f);
    check (c == 0 && std::abs (p.getBand (0).gain) < 1.0e-6f, "re-added slot starts clean");

    const auto text = p.bandsToText ({ 0, 1 });
    const auto pasted = p.pasteBands (text);
    check (pasted.size() == 2 && p.getNumUsedBands() == 4 && p.getBand (pasted[1]).shape == Shape::highCut, "copy / paste bands");

    juce::MemoryBlock state;
    setParam (p, ids::band (1, "freq"), 7777.0f);
    p.getStateInformation (state);
    StEqProcessor p2;
    p2.setStateInformation (state.getData(), (int) state.getSize());
    check (std::abs (p2.getBand (1).freq - 7777.0f) < 1.0f && p2.getNumUsedBands() == 4, "state round trip");

    int loaded = 0;
    for (int i = 0; i < (int) p.presets.getPresets().size(); ++i)
    {
        p.presets.loadPreset (i);
        bool finite = true;
        p.refreshFirNow();
        const float peak = runNoise (p, 4, 512, finite);
        check (finite && peak < 50.0f, "preset " + p.presets.getPresets()[(size_t) i].name);
        ++loaded;
    }
    std::cout << "  " << loaded << " presets processed\n";
    p.presets.loadPreset (0);
}

static void testCpu (StEqProcessor& p)
{
    std::cout << "CPU\n";
    auto measure = [&p] (const char* what)
    {
        bool finite = true;
        const int blocks = 300;
        runNoise (p, 20, 512, finite);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        runNoise (p, blocks, 512, finite);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::cout << "  " << what << ": " << juce::String (100.0 * ms / (blocks * 512 / 48.0), 2) << "% of one core\n";
    };

    clearBands (p);
    for (int b = 0; b < 12; ++b) p.addBand ((Shape) (b % 5), 40.0f * std::pow (1.6f, (float) b), (b % 2 ? 4.0f : -3.0f), 1.0f, 3);
    setMode (p, ProcMode::zeroLatency); measure ("12 bands, Zero Latency");
    for (int b = 0; b < 12; b += 2) setParam (p, ids::band (b, "dyn_range"), -6.0f);
    measure ("12 bands (6 dynamic), Zero Latency");
    setMode (p, ProcMode::naturalPhase); measure ("12 bands (6 dynamic), Natural Phase");
    setMode (p, ProcMode::linearPhase, 1); measure ("12 bands (6 dynamic), Linear Phase Medium");
    setMode (p, ProcMode::linearPhase, 4); measure ("12 bands (6 dynamic), Linear Phase Maximum");
    for (int b = 12; b < 24; ++b) p.addBand (Shape::bell, 30.0f * std::pow (1.5f, (float) (b - 12)), 2.0f, 2.0f, 1, Placement::mid);
    measure ("24 bands incl. mid, Linear Phase Maximum (matrix)");
    setMode (p, ProcMode::zeroLatency);
}


//==============================================================================
namespace
{
struct MouseSim
{
    juce::Component& c;
    juce::Point<float> down;

    juce::MouseEvent ev (juce::Point<float> pos, juce::ModifierKeys mods, int clicks = 1, bool dragged = false)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, down, now, clicks, dragged);
    }
    void click (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        down = p;
        const auto m = mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        c.mouseDown (ev (p, m));
        c.mouseUp (ev (p, mods));
    }
    void doubleClick (juce::Point<float> p, juce::ModifierKeys mods = {})
    {
        click (p, mods);
        c.mouseDoubleClick (ev (p, mods, 2));
    }
    void drag (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {}, int steps = 8)
    {
        down = from;
        const auto m = mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        c.mouseMove (ev (from, mods));
        c.mouseDown (ev (from, m));
        for (int i = 1; i <= steps; ++i)
            c.mouseDrag (ev (from + (to - from) * ((float) i / (float) steps), m, 1, true));
        c.mouseUp (ev (to, mods, 1, true));
    }
    void wheel (juce::Point<float> p, float dy, juce::ModifierKeys mods = {})
    {
        juce::MouseWheelDetails w { 0.0f, dy, false, false, false };
        c.mouseMove (ev (p, mods));
        c.mouseWheelMove (ev (p, mods), w);
    }
};

float hzToX (EqDisplay& d, float hz) { return d.xForHz (hz); }
}

static void testInteraction (StEqProcessor& p)
{
    std::cout << "Display interaction\n";
    clearBands (p);
    p.presets.loadPreset (0);
    p.uiState.setProperty ("selection", "", nullptr);
    p.uiState.setProperty ("range", 2, nullptr);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (StEqEditor::designWidth, StEqEditor::designHeight);
    auto& e = *dynamic_cast<StEqEditor*> (ed.get());
    auto& d = e.getDisplay();
    d.setDisplayRange (2);
    d.syncFromParameters();
    MouseSim m { d, {} };
    using MK = juce::ModifierKeys;

    // double-click creates a bell where you click
    m.doubleClick ({ hzToX (d, 1000.0f), d.yForDb (6.0f) });
    check (p.getNumUsedBands() == 1, "double-click creates a band");
    auto b0 = p.getBand (0);
    check (b0.shape == Shape::bell && std::abs (std::log2 (b0.freq / 1000.0f)) < 0.05f && std::abs (b0.gain - 6.0f) < 0.4f,
           "new bell at 1 kHz / +6 dB (" + juce::String (b0.freq, 1) + " Hz, " + juce::String (b0.gain, 2) + " dB)");
    check (d.getSelection().size() == 1 && d.getPrimary() == 0, "new band is selected");
    check (e.getBandPanel() != nullptr && e.getBandPanel()->getBand() == 0, "band island shows the new band");
    {
        // regression: the curve of a band created by a mouse action must be drawn right away
        juce::Image img (juce::Image::ARGB, d.getWidth(), d.getHeight(), true);
        { juce::Graphics g (img); d.paintEntireComponent (g, true); }
        const float yCurve = d.getCurveY (1000.0f);
        check (std::abs (yCurve - d.yForDb (6.0f)) < 3.0f, "new band's curve is drawn immediately (" + juce::String (d.dbForY (yCurve), 2) + " dB at 1 kHz)");
    }

    // drag the dot: +1 octave, down to +3 dB
    d.syncFromParameters();
    const auto dot = d.dotPosition (0);
    const float octPx = hzToX (d, 2000.0f) - hzToX (d, 1000.0f);
    m.drag (dot, { dot.x + octPx, d.yForDb (3.0f) });
    b0 = p.getBand (0);
    check (std::abs (std::log2 (b0.freq / 2000.0f)) < 0.05f && std::abs (b0.gain - 3.0f) < 0.3f,
           "drag moves frequency and gain (" + juce::String (b0.freq, 1) + " Hz, " + juce::String (b0.gain, 2) + " dB)");

    // shift-drag is fine
    d.syncFromParameters();
    const float before = p.getBand (0).freq;
    m.drag (d.dotPosition (0), d.dotPosition (0).translated (octPx, 0.0f), MK (MK::shiftModifier));
    const float ratio = p.getBand (0).freq / before;
    check (ratio > 1.05f && ratio < 1.3f, "shift-drag is fine (" + juce::String (ratio, 3) + "x for one octave of mouse)");

    // alt-drag locks the axis
    d.syncFromParameters();
    const float g0 = p.getBand (0).gain;
    m.drag (d.dotPosition (0), d.dotPosition (0).translated (octPx * 0.5f, 8.0f), MK (MK::altModifier));
    check (std::abs (p.getBand (0).gain - g0) < 0.01f, "alt-drag constrains to the dominant axis");

    // wheel = Q, alt + wheel = dynamic range
    d.syncFromParameters();
    const float q0 = p.getBand (0).q;
    m.wheel (d.dotPosition (0), 0.5f);
    check (p.getBand (0).q > q0 * 1.3f, "wheel raises Q");
    m.wheel (d.dotPosition (0), -0.5f, MK (MK::altModifier));
    check (p.getBand (0).dynRange < -1.0f, "alt + wheel makes the band dynamic");

    // dynamic range handle drag
    d.syncFromParameters();
    {
        const auto bp = p.getBand (0);
        const juce::Point<float> handle (d.dotPosition (0).x, d.yForDb (bp.gain + bp.dynRange));
        m.drag (handle, handle.translated (0.0f, d.yForDb (0.0f) - d.yForDb (3.0f)));
        check (std::abs (p.getBand (0).dynRange - (bp.dynRange - 3.0f)) < 0.4f,
               "range handle drag (" + juce::String (p.getBand (0).dynRange, 2) + " dB)");
    }

    // alt-click toggles bypass
    d.syncFromParameters();
    m.click (d.dotPosition (0), MK (MK::altModifier));
    check (p.getBand (0).bypass, "alt-click bypasses");
    d.syncFromParameters();
    m.click (d.dotPosition (0), MK (MK::altModifier));
    check (! p.getBand (0).bypass, "alt-click again re-enables");

    // drag the curve to create a band (gain follows the mouse)
    d.syncFromParameters();
    {
        const float x = hzToX (d, 150.0f);
        const juce::Point<float> onCurve (x, d.yForDb (0.0f));
        m.drag (onCurve, { x, d.yForDb (-4.0f) });
        check (p.getNumUsedBands() == 2, "dragging the curve creates a band");
        const auto nb = p.getBand (1);
        check (nb.shape == Shape::bell && std::abs (nb.gain + 4.0f) < 0.4f, "curve-drag band follows the mouse (" + juce::String (nb.gain, 2) + " dB)");
    }

    // double-click at the far left creates a low cut
    m.doubleClick ({ hzToX (d, 15.0f), d.yForDb (0.0f) });
    check (p.getNumUsedBands() == 3 && p.getBand (2).shape == Shape::lowCut, "far-left double-click makes a low cut");

    // rubber band selects, cmd-A selects all
    d.syncFromParameters();
    {
        const auto a = d.dotPosition (0), b = d.dotPosition (1);
        const juce::Rectangle<float> r (a, b);
        m.drag (r.getTopLeft().translated (-20.0f, -20.0f), r.getBottomRight().translated (20.0f, 20.0f));
        check (d.getSelection().contains (0) && d.getSelection().contains (1) && ! d.getSelection().contains (2), "rubber band selection");
    }
    d.keyPressed (juce::KeyPress ('a', MK (MK::commandModifier), 0));
    check (d.getSelection().size() == 3, "cmd-A selects all");

    // multi-band drag moves all selected bands together
    d.syncFromParameters();
    {
        const float f0 = p.getBand (0).freq, f1 = p.getBand (1).freq;
        m.drag (d.dotPosition (1), d.dotPosition (1).translated (octPx, 0.0f));
        check (std::abs (std::log2 (p.getBand (0).freq / f0) - 1.0f) < 0.06f && std::abs (std::log2 (p.getBand (1).freq / f1) - 1.0f) < 0.06f,
               "selected bands move together");
    }

    // copy / paste, delete, undo
    d.keyPressed (juce::KeyPress ('c', MK (MK::commandModifier), 0));
    d.keyPressed (juce::KeyPress ('v', MK (MK::commandModifier), 0));
    check (p.getNumUsedBands() == 6, "cmd-C / cmd-V duplicates the selection (" + juce::String (p.getNumUsedBands()) + ")");
    d.syncFromParameters();
    d.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
    check (p.getNumUsedBands() == 3, "delete removes the selected bands (" + juce::String (p.getNumUsedBands()) + ")");
    // (undo is recorded by the APVTS flush timer, which needs a message loop: not testable in this console app)

    // EQ Sketch: draw a bump around 2 kHz
    clearBands (p);
    d.syncFromParameters();
    d.setSketchMode (true);
    {
        std::vector<juce::Point<float>> stroke;
        for (int i = 0; i <= 60; ++i)
        {
            const float hz = 100.0f * std::pow (200.0f, (float) i / 60.0f);
            const float db = 7.0f * std::exp (-std::pow (std::log2 (hz / 2000.0f) / 0.9f, 2.0f));
            stroke.push_back ({ hzToX (d, hz), d.yForDb (db) });
        }
        m.down = stroke.front();
        d.mouseDown (m.ev (stroke.front(), MK (MK::leftButtonModifier)));
        for (auto& pt : stroke) d.mouseDrag (m.ev (pt, MK (MK::leftButtonModifier), 1, true));
        d.mouseUp (m.ev (stroke.back(), {}, 1, true));

        double worst = 0.0;
        for (float hz : { 300.0f, 1000.0f, 2000.0f, 4000.0f, 10000.0f })
        {
            double db = 0.0;
            for (int b = 0; b < kMaxBands; ++b)
                if (p.getBand (b).isActive()) db += st::dsp::eq::designAnalog (makeSpec (p.getBand (b), 1.0f)).magnitudeDb (hz);
            const double want = 7.0 * std::exp (-std::pow (std::log2 (hz / 2000.0) / 0.9, 2.0));
            worst = std::max (worst, std::abs (db - want));
        }
        check (p.getNumUsedBands() >= 1 && ! d.isSketchMode(), "sketch creates bands and exits sketch mode");
        check (worst < 1.0, "sketched curve matches the stroke (worst " + juce::String (worst, 2) + " dB, " + juce::String (p.getNumUsedBands()) + " bands)");
    }

    // value entry: double-click a dot, type a note name
    clearBands (p);
    d.syncFromParameters();
    m.doubleClick ({ hzToX (d, 500.0f), d.yForDb (3.0f) });
    d.syncFromParameters();
    m.doubleClick (d.dotPosition (0));
    if (auto* te = dynamic_cast<juce::TextEditor*> (d.getChildComponent (d.getNumChildComponents() - 1)))
    {
        te->setText ("A4");
        te->onReturnKey();
        check (std::abs (p.getBand (0).freq - 440.0f) < 0.5f, "typing a note name sets the frequency (" + juce::String (p.getBand (0).freq, 1) + ")");
    }
    else check (false, "double-click on a dot opens value entry");

    // progressive EQ Sketch: bands appear while drawing; the stroke ends up as bands matching it
    clearBands (p);
    d.syncFromParameters();
    d.setSketchMode (true);
    {
        const MK lb (MK::leftButtonModifier);
        auto at = [&] (float hz, float db) { return juce::Point<float> (hzToX (d, hz), d.yForDb (db)); };
        m.down = at (100.0f, 0.0f);
        d.mouseDown (m.ev (m.down, lb));
        for (int i = 1; i <= 40; ++i)
        {
            const float hz = 100.0f * std::pow (100.0f, (float) i / 40.0f);
            const float db = 6.0f * std::exp (-std::pow (std::log2 (hz / 1000.0f) / 0.7f, 2.0f));
            d.mouseDrag (m.ev (at (hz, db), lb, 1, true));
            if (i == 30) juce::Thread::sleep (130);
        }
        check (p.getNumUsedBands() > 0, "sketch: bands appear while drawing");
        d.mouseUp (m.ev (at (10000.0f, 0.0f), {}, 1, true));
        d.syncFromParameters();
        const float at1k = d.dbForY (d.getCurveY (1000.0f));
        check (! d.isSketchMode() && std::abs (at1k - 6.0f) < 1.5f, "sketch: drawn bump becomes the curve (" + juce::String (at1k, 2) + " dB at 1 kHz)");
    }

    // parameter display: hover the dot, then use its values and buttons
    clearBands (p);
    d.syncFromParameters();
    m.doubleClick ({ hzToX (d, 1000.0f), d.yForDb (4.0f) });
    d.syncFromParameters();
    {
        auto hover = [&] { d.mouseMove (m.ev (d.dotPosition (0), {})); };
        hover();
        const auto gainBox = d.getReadoutPart (0, "gain");
        check (! gainBox.isEmpty(), "parameter display has a gain value");
        const float gain0 = p.getBand (0).gain;
        d.mouseMove (m.ev (gainBox.getCentre(), {}));   // crossing onto the display keeps it
        m.drag (gainBox.getCentre(), gainBox.getCentre().translated (0.0f, -20.0f));
        check (std::abs (p.getBand (0).gain - (gain0 + 2.0f)) < 0.3f, "dragging the gain value up (" + juce::String (p.getBand (0).gain, 2) + " dB)");

        check (std::abs (std::log2 (p.getBand (0).freq / 1000.0f)) < 0.05f, "a committed value entry is not applied again by the next click");
        hover();
        const auto solo = d.getReadoutPart (0, "solo");
        d.mouseMove (m.ev (solo.getCentre(), {}));
        m.down = solo.getCentre();
        d.mouseDown (m.ev (solo.getCentre(), MK (MK::leftButtonModifier)));
        check (p.soloBand.load() == 0, "holding the headphones solos the band");
        d.mouseDrag (m.ev (solo.getCentre().translated (hzToX (d, 2000.0f) - hzToX (d, 1000.0f), 0.0f), MK (MK::leftButtonModifier), 1, true));
        check (std::abs (std::log2 (p.getBand (0).freq / 2000.0f)) < 0.08f, "dragging while soloing moves the band (" + juce::String (p.getBand (0).freq, 0) + " Hz)");
        d.mouseUp (m.ev (solo.getCentre(), {}, 1, true));
        check (p.soloBand.load() == -1, "releasing ends the solo");

        hover();
        const auto byp = d.getReadoutPart (0, "bypass");
        d.mouseMove (m.ev (byp.getCentre(), {}));
        m.click (byp.getCentre());
        check (p.getBand (0).bypass, "bypass button in the parameter display");

        hover();
        const auto rm = d.getReadoutPart (0, "remove");
        d.mouseMove (m.ev (rm.getCentre(), {}));
        m.click (rm.getCentre());
        check (p.getNumUsedBands() == 0, "delete button in the parameter display");
    }

    ed.reset();
    clearBands (p);
}

static void testInstancesAndOutput (StEqProcessor& p)
{
    std::cout << "Typed values\n";
    {
        auto& gain = p.param (ids::band (0, "gain"));
        auto& freq = p.param (ids::band (0, "freq"));
        auto plain = [] (juce::RangedAudioParameter& prm, const char* text) { return prm.convertFrom0to1 (prm.getValueForText (text)); };
        check (std::abs (plain (gain, "2x") - 6.02f) < 0.05f, "gain '2x' = +6 dB");
        check (std::abs (plain (gain, "50%") + 6.02f) < 0.05f, "gain '50%' = -6 dB");
        check (std::abs (plain (gain, "-3.5 dB") + 3.5f) < 0.02f, "gain '-3.5 dB'");
        check (std::abs (plain (freq, "A4") - 440.0f) < 0.5f, "freq 'A4' = 440 Hz");
        check (std::abs (plain (freq, "C#3+13") - 140.0f) < 1.5f, "freq 'C#3+13' (" + juce::String (plain (freq, "C#3+13"), 1) + ")");
        check (std::abs (plain (freq, "1.5k") - 1500.0f) < 1.0f, "freq '1.5k'");
        check (st::Knob::doubleClickEntersValue(), "ST EQ knobs: double-click types a value");
    }

    std::cout << "Instances / bypass / output\n";
    auto& reg = st::InstanceRegistry::get();
    const int before = reg.getNumInstances();
    check (before >= 1 && reg.getInstances().contains (&p), "processor registered");

    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    auto* e = dynamic_cast<StEqEditor*> (ed.get());
    {
        auto other = std::make_unique<StEqProcessor>();
        other->setPlayConfigDetails (2, 2, 48000.0, 512);
        other->prepareToPlay (48000.0, 512);
        juce::AudioProcessor::TrackProperties tp;
        tp.name = "Bass";
        tp.colourARGB = 0xff3080ff;
        other->updateTrackProperties (tp);
        reg.flush();
        check (reg.getNumInstances() == before + 1 && other->getInstanceLabel() == "Bass", "second instance registered with its track name");
        check (e->getInstanceHub().getOthers().size() == 1, "hub sees the other instance");

        // other instance plays something: its spectrum is readable without consuming its own analyser feed
        other->addBand (Shape::bell, 200.0f, 6.0f, 1.0f);
        bool finite = true;
        runNoise (*other, 40, 512, finite);
        e->openInstances();
        e->getInstanceHub().update (0.5, e->getDisplay().getSpectrum (1), true);
        const auto& lv = e->getInstanceHub().getOthers()[0]->spectrum->getLevelsDb();
        const float loudest = *std::max_element (lv.begin(), lv.end());
        check (loudest > -80.0f, "remote spectrum read (" + juce::String (loudest, 1) + " dB)");

        // multiview: a row per track, a module per plug-in, each editable
        auto* mv = e->getMultiView();
        check (mv != nullptr && mv->getNumRows() == 2 && mv->getNumModules() == 2, "multiview: a row per track");
        check (mv != nullptr && mv->getParentComponent() != nullptr && mv->getBounds() == mv->getParentComponent()->getLocalBounds(),
               "multiview covers the whole plugin window");
        auto* strip = mv != nullptr ? dynamic_cast<EqStrip*> (mv->getModuleComponent (other->getInstanceUid())) : nullptr;
        check (strip != nullptr && strip->getDisplay().isCompact(), "editable EQ module for the other instance");
        if (strip != nullptr)
        {
            // edit the other track from here: double-click its display creates a band there
            auto& od = strip->getDisplay();
            od.syncFromParameters();
            MouseSim ms { od, {} };
            const int before2 = other->getNumUsedBands();
            ms.doubleClick ({ od.xForHz (3000.0f), od.yForDb (5.0f) });
            check (other->getNumUsedBands() == before2 + 1 && p.getNumUsedBands() == 0, "editing a module edits that track only");
            MouseSim mb { strip->getBypassSwitch(), {} };
            mb.click ({ 8.0f, 8.0f });
            check (other->refs.bypass.getBool() && ! p.refs.bypass.getBool(), "module bypass switch bypasses that track");
            mb.click ({ 8.0f, 8.0f });
        }

        // a second plug-in on the same track joins that row
        auto sameTrack = std::make_unique<StEqProcessor>();
        sameTrack->updateTrackProperties (tp);
        reg.flush();
        check (mv->getNumRows() == 2 && mv->getNumModules() == 3, "same track: one row, modules side by side");
        sameTrack.reset();
        check (mv->getNumModules() == 2, "removed module dropped synchronously");

        mv->setHoveredInstance (other->getInstanceUid());
        juce::Image wimg (juce::Image::ARGB, mv->getWidth(), mv->getHeight(), true);
        for (int i = 0; i < 40; ++i) { juce::Graphics g (wimg); mv->paintEntireComponent (g, true); }

        // destroy the other instance while the multiview shows it: its module goes first, no crash
        mv->setHoveredInstance (0);
        other.reset();
        check (e->getInstanceHub().getOthers().empty() && mv->getNumRows() == 1, "removed instance's row dropped synchronously");
        { juce::Graphics g (wimg); mv->paintEntireComponent (g, true); }
    }
    e->openOutput();
    ed.reset();

    // global bypass: output = input delayed by the latency (linear phase), even with bands
    clearBands (p);
    p.addBand (Shape::bell, 1000.0f, 9.0f, 1.0f);
    setMode (p, ProcMode::linearPhase, 0);
    p.refreshFirNow();
    setParam (p, ids::bypass, 1.0f);
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        juce::Random rng (4);
        std::vector<float> in, out;
        p.getEngine().reset();
        for (int blk = 0; blk < 40; ++blk)
        {
            for (int i = 0; i < 512; ++i) { const float v = rng.nextFloat() - 0.5f; buf.setSample (0, i, v); buf.setSample (1, i, v); in.push_back (v); }
            p.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) out.push_back (buf.getSample (0, i));
        }
        const int lat = p.getLatencySamples();
        double err = 0.0;
        for (size_t i = (size_t) lat + 4096; i < out.size(); ++i) err = std::max (err, (double) std::abs (out[i] - in[i - (size_t) lat]));
        check (err < 1.0e-6, "global bypass = latency-compensated dry (" + juce::String (err, 8) + ")");
    }
    setParam (p, ids::bypass, 0.0f);
    setMode (p, ProcMode::zeroLatency);

    // mid / side pan: full "right" in M/S mode removes the mid (a centred signal goes silent)
    clearBands (p);
    setParam (p, ids::panMode, 1.0f);
    setParam (p, ids::outPan, 100.0f);
    const double centred = sineGainDb (p, 1000.0, -12.0, 48000.0, 1);
    check (centred < -60.0, "M/S pan removes the mid (" + juce::String (centred, 1) + " dB)");
    setParam (p, ids::outPan, 0.0f);
    setParam (p, ids::panMode, 0.0f);
}

#if JUCE_MAC
/** Loads the installed ST Sat AU into this process (a different binary, its own JUCE) and checks that ST EQ's
    multiview shows it on the same track row as ST EQ (EQ leftmost), renders it and drives it remotely. */
static void testCrossPlugin (StEqProcessor& p)
{
    std::cout << "Cross-plugin multiview (ST Sat AU in this process)\n";
    AudioComponentDescription desc {};
    desc.componentType = kAudioUnitType_Effect;
    desc.componentSubType = 'Stst';
    desc.componentManufacturer = 'StAu';
    AudioComponent comp = AudioComponentFindNext (nullptr, &desc);
    check (comp != nullptr, "ST Sat AU is installed");
    if (comp == nullptr) return;
    AudioUnit au = nullptr;
    check (AudioComponentInstanceNew (comp, &au) == noErr && au != nullptr, "ST Sat AU instantiated in-process");
    if (au == nullptr) return;

    auto& reg = st::InstanceRegistry::get();
    reg.flush();
    st::InstanceRegistry::Info sat;
    for (auto& i : reg.getAllInstances()) if (i.kind == "ST Sat") sat = i;
    check (sat.uid != 0 && sat.local == nullptr && sat.api != nullptr, "ST Sat (other binary) visible in the shared registry");

    // same track: the host names both (Logic sends the track name to AUs as the context name)
    CFStringRef name = CFSTR ("Vocal");
    AudioUnitSetProperty (au, kAudioUnitProperty_ContextName, kAudioUnitScope_Global, 0, &name, sizeof (name));
    juce::AudioProcessor::TrackProperties tp;
    tp.name = juce::String ("Vocal");
    p.updateTrackProperties (tp);
    reg.flush();

    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    ed->setSize (StEqEditor::designWidth, StEqEditor::designHeight);
    auto* e = dynamic_cast<StEqEditor*> (ed.get());
    e->openInstances();
    auto* mv = e->getMultiView();
    mv->refresh();
    int vocalRow = -1;
    for (int r = 0; r < mv->getNumRows(); ++r) if (mv->getRowKinds (r).contains ("ST Sat")) vocalRow = r;
    const auto kinds = mv->getRowKinds (vocalRow);
    check (kinds.size() == 2 && kinds[0] == "ST EQ" && kinds[1] == "ST Sat", "same track row: ST EQ leftmost, then ST Sat (" + kinds.joinIntoString (", ") + ")");
    check (mv->getModuleComponent (sat.uid) != nullptr, "ST Sat module shown (remote view)");
    {
        juce::Image img (juce::Image::ARGB, mv->getWidth(), mv->getHeight(), true);
        juce::Graphics g (img);
        mv->paintEntireComponent (g, true);
    }

    // the module rendered by ST Sat's own code
    const auto* api = sat.api;
    const uint64_t view = api->createView (sat.ctx);
    check (view != 0, "ST Sat provides a module view");
    if (view != 0)
    {
        const int H = 160, W = api->preferredWidth (view, H);
        api->setViewSize (view, W, H);
        std::vector<uint32_t> px ((size_t) (W * H), 0u);
        check (api->render (view, px.data(), W, H, W * 4, 1.0f) == 1, "remote render");
        const auto opaque = std::count_if (px.begin(), px.end(), [] (uint32_t v) { return (v >> 24) != 0; });
        check (opaque > (long) (W * H) / 2, "remote module drew its content (" + juce::String ((int) opaque) + " px)");

        // drag band 1's drive knob up through the forwarded mouse; ST Sat's parameter must follow
        AudioUnitParameterID driveId = 0;
        bool found = false;
        UInt32 size = 0;
        if (AudioUnitGetPropertyInfo (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr) == noErr && size > 0)
        {
            std::vector<AudioUnitParameterID> ids (size / sizeof (AudioUnitParameterID));
            AudioUnitGetProperty (au, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
            for (auto id : ids)
            {
                AudioUnitParameterInfo info {};
                UInt32 isz = sizeof (info);
                if (AudioUnitGetProperty (au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &isz) != noErr) continue;
                if (info.cfNameString != nullptr)
                {
                    const auto n = juce::String::fromCFString (info.cfNameString);
                    if ((info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0) CFRelease (info.cfNameString);
                    if (n == "Band 1 Drive") { driveId = id; found = true; }
                }
            }
        }
        check (found, "ST Sat drive parameter found");
        AudioUnitParameterValue before = 0, after = 0;
        AudioUnitGetParameter (au, driveId, kAudioUnitScope_Global, 0, &before);
        const int knobs = (int) ((W - 42) / juce::jlimit (54, 110, juce::roundToInt ((float) H * 0.62f)));
        const float kw = (float) juce::jmin ((W - 24) / juce::jmax (1, knobs), juce::jlimit (54, 110, juce::roundToInt ((float) H * 0.62f)));
        const float x0 = 12.0f + ((float) (W - 24) - kw * (float) knobs) * 0.5f + kw * 0.5f, y0 = (float) H * 0.5f;
        api->mouse (view, st::shared::mouseMove, x0, y0, 0, 0, 0, 1);
        api->mouse (view, st::shared::mouseDown, x0, y0, st::shared::modLeft, 0, 0, 1);
        for (int i = 1; i <= 10; ++i) api->mouse (view, st::shared::mouseDrag, x0, y0 - 6.0f * (float) i, st::shared::modLeft, 0, 0, 1);
        api->mouse (view, st::shared::mouseUp, x0, y0 - 60.0f, 0, 0, 0, 1);
        AudioUnitGetParameter (au, driveId, kAudioUnitScope_Global, 0, &after);
        check (after > before + 0.1f, "dragging the remote knob changes ST Sat's drive (normalised " + juce::String (before, 2) + " -> " + juce::String (after, 2) + ")");
        api->destroyView (view);
    }

    // ST Sat goes away: its module leaves the row, nothing calls into it any more
    AudioComponentInstanceDispose (au);
    reg.flush();
    check (! reg.isAlive (sat.uid) && mv->getModuleComponent (sat.uid) == nullptr, "disposed ST Sat leaves the multiview");
    {
        juce::Image img (juce::Image::ARGB, mv->getWidth(), mv->getHeight(), true);
        juce::Graphics g (img);
        mv->paintEntireComponent (g, true);
    }
    ed.reset();
    p.updateTrackProperties ({});
}
#endif

static int runTests()
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto proc = std::make_unique<StEqProcessor>();
    auto& p = *proc;
    p.setPlayConfigDetails (2, 2, 48000.0, 512);
    p.prepareToPlay (48000.0, 512);

    testResponses (p);
    testDynamics (p);
    testSpectral (p);
    testStress (p);
    testState (p);
    testCpu (p);
    testInteraction (p);
    testInstancesAndOutput (p);
   #if JUCE_MAC
    testCrossPlugin (p);
   #endif

    // UI: full-editor repaint cost at 2x (retina) with 12 bands, analyser and a selection
    {
        clearBands (p);
        for (int b = 0; b < 12; ++b) p.addBand ((Shape) (b % 6), 40.0f * std::pow (1.7f, (float) b), (b % 2 ? 4.0f : -3.0f), 1.0f, 3);
        setParam (p, ids::band (2, "dyn_range"), -6.0f);
        p.uiState.setProperty ("selection", "2,3", nullptr);
        p.uiState.setProperty ("primary", 3, nullptr);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (StEqEditor::designWidth, StEqEditor::designHeight);
        juce::Image img (juce::Image::ARGB, StEqEditor::designWidth * 2, StEqEditor::designHeight * 2, true);
        auto frame = [&]
        {
            juce::Graphics g (img);
            g.addTransform (juce::AffineTransform::scale (2.0f));
            ed->paintEntireComponent (g, true);
        };
        frame();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        constexpr int frames = 40;
        for (int i = 0; i < frames; ++i) frame();
        const double ms = (juce::Time::getMillisecondCounterHiRes() - t0) / frames;
        std::cout << "UI: full editor repaint at 2x: " << juce::String (ms, 2) << " ms per frame\n";
        if (auto* e = dynamic_cast<StEqEditor*> (ed.get()))
        {
            auto timeOne = [&] (juce::Component& c, const char* name)
            {
                juce::Image im (juce::Image::ARGB, c.getWidth() * 2, c.getHeight() * 2, true);
                const auto t1 = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < frames; ++i) { juce::Graphics g (im); g.addTransform (juce::AffineTransform::scale (2.0f)); c.paintEntireComponent (g, true); }
                std::cout << "   " << name << ": " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t1) / frames, 2) << " ms\n";
            };
            timeOne (e->getDisplay(), "display");
            if (auto* bp = e->getBandPanel()) timeOne (*bp, "band panel");
        }
        check (ms < 16.0, "full repaint fits a 60 fps frame");
    }

    std::cout << checks << " checks, " << (failures == 0 ? juce::String ("ALL PASSED") : juce::String (failures) + " FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}

//==============================================================================
namespace
{
/** Music-like test signal: pink-ish noise plus a few tones, so the analyser has something to show. */
void fillSignal (juce::AudioBuffer<float>& buf, int64_t& pos, juce::Random& rng, float* lp)
{
    for (int i = 0; i < buf.getNumSamples(); ++i, ++pos)
    {
        const double t = (double) pos / 48000.0;
        float v = 0.0f;
        for (int c = 0; c < 3; ++c) { lp[c] += (0.02f + 0.2f * (float) c) * ((rng.nextFloat() * 2.0f - 1.0f) - lp[c]); v += lp[c] * (0.6f / (float) (c + 1)); }
        v += 0.18f * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * t);
        v += 0.06f * (float) std::sin (juce::MathConstants<double>::twoPi * 880.0 * t);
        v += 0.03f * (float) std::sin (juce::MathConstants<double>::twoPi * 3150.0 * t);
        buf.setSample (0, i, v * 0.3f);
        buf.setSample (1, i, v * 0.27f);
    }
}

/** Renders the editor after letting its timers run for a while (needs a running message loop). */
struct Snapshotter : juce::Timer
{
    struct Job { std::function<void()> setup; juce::String name; float scale = 1.0f; std::function<void (StEqEditor&)> afterOpen; };
    StEqProcessor& p;
    juce::File dir;
    std::vector<Job> jobs;
    size_t index = 0;
    int ticks = 0;
    bool opened = false;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    juce::Random rng { 5 };
    int64_t pos = 0;
    float lp[3] {};

    std::function<void()> onTick;
    Snapshotter (StEqProcessor& proc, juce::File d) : p (proc), dir (d) {}

    void timerCallback() override
    {
        if (ed == nullptr)
        {
            if (index >= jobs.size()) { stopTimer(); juce::JUCEApplicationBase::quit(); return; }
            if (jobs[index].setup) jobs[index].setup();
            p.applyModeChange();
            ed.reset (p.createEditor());
            ed->setSize (juce::roundToInt ((float) StEqEditor::designWidth * jobs[index].scale),
                         juce::roundToInt ((float) StEqEditor::designHeight * jobs[index].scale));
            ticks = 0;
            opened = false;
        }

        juce::AudioBuffer<float> buf (2, 800);
        juce::MidiBuffer midi;
        fillSignal (buf, pos, rng, lp);
        p.processBlock (buf, midi);
        if (onTick) onTick();

        if (! opened && ticks > 6 && jobs[index].afterOpen)
            if (auto* e = dynamic_cast<StEqEditor*> (ed.get())) { jobs[index].afterOpen (*e); opened = true; }

        if (++ticks > 70)
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            auto file = dir.getChildFile (jobs[index].name + ".png");
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (img, out);
            std::cout << "wrote " << file.getFullPathName() << "\n";
            if (auto* e = dynamic_cast<StEqEditor*> (ed.get()))
                if (auto* bp = e->getBandPanel(); bp != nullptr && jobs[index].scale == 1.0f)
                {
                    auto crop = ed->createComponentSnapshot (bp->getBounds().expanded (6), true, 4.0f);
                    auto f2 = dir.getChildFile (jobs[index].name + "_island.png");
                    f2.deleteFile();
                    juce::FileOutputStream o2 (f2);
                    juce::PNGImageFormat().writeImageToStream (crop, o2);
                }
            ed.reset();
            ++index;
        }
    }
};

juce::File snapshotDir;

struct SnapshotApp : juce::JUCEApplicationBase
{
    std::unique_ptr<StEqProcessor> proc;
    std::vector<std::unique_ptr<StEqProcessor>> extras;
   #if JUCE_MAC
    std::vector<AudioUnit> satUnits;
   #endif
    int64_t extraPos[4] {};
    juce::Random extraRng[4] { juce::Random (11), juce::Random (12), juce::Random (13), juce::Random (14) };
    float extraLp[4][3] {};
    std::unique_ptr<Snapshotter> snap;

    const juce::String getApplicationName() override { return "StEqSnapshots"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted (const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void systemRequestedQuit() override { quit(); }
    void unhandledException (const std::exception*, const juce::String&, int) override {}
    void shutdown() override
    {
        snap.reset();
       #if JUCE_MAC
        for (auto au : satUnits) AudioComponentInstanceDispose (au);
        satUnits.clear();
       #endif
        extras.clear();
        proc.reset();
    }

    void initialise (const juce::String&) override
    {
        proc = std::make_unique<StEqProcessor>();
        auto& p = *proc;
        p.setPlayConfigDetails (2, 2, 48000.0, 800);
        p.prepareToPlay (48000.0, 800);
        snapshotDir.createDirectory();

        auto load = [&p] (const juce::String& name)
        {
            for (int i = 0; i < (int) p.presets.getPresets().size(); ++i)
                if (p.presets.getPresets()[(size_t) i].name == name) p.presets.loadPreset (i);
        };
        auto selectBands = [&p] (juce::String sel, int prim)
        {
            p.uiState.setProperty ("selection", sel, nullptr);
            p.uiState.setProperty ("primary", prim, nullptr);
        };

        snap = std::make_unique<Snapshotter> (p, snapshotDir);
        snap->jobs.push_back ({ [&p, selectBands] { p.presets.loadPreset (0); selectBands ("", -1); }, "01_init" });
        snap->jobs.push_back ({ [load, selectBands] { load ("Vocal Presence"); selectBands ("2", 2); }, "02_vocal_presence" });
        snap->jobs.push_back ({ [load, selectBands] { load ("Vocal De-Harsh"); selectBands ("1", 1); }, "03_dynamic_band" });
        snap->jobs.push_back ({ [load, selectBands] { load ("Mid/Side Width"); selectBands ("0,1", 1); }, "04_mid_side_multi" });
        snap->jobs.push_back ({ [load, selectBands] { load ("Vocal De-Harsh"); selectBands ("1", 1); }, "05_dynamics_popover", 1.0f,
                                [] (StEqEditor& e) { e.openDynamicsPopover(); } });
        snap->jobs.push_back ({ [load, selectBands, &p]
        {
            load ("Vocal De-Harsh");
            p.setBandValue (1, "dyn_trig", 1.0f);
            p.setBandValue (1, "dyn_lo", 2000.0f);
            p.setBandValue (1, "dyn_hi", 6000.0f);
            selectBands ("1", 1);
        }, "05b_dynamics_free_trigger", 1.0f, [] (StEqEditor& e) { e.openDynamicsPopover(); } });
        snap->jobs.push_back ({ [load, selectBands] { load ("Vocal Resonance Tamer"); selectBands ("1", 1); }, "05c_spectral", 1.0f,
                                [] (StEqEditor& e) { e.openDynamicsPopover(); } });
        snap->jobs.push_back ({ [load, selectBands] { load ("Kick Punch"); selectBands ("1", 1); }, "06_small_scale", 0.75f });
        snap->jobs.push_back ({ [&p, load, selectBands]
        {
            load ("Mastering Air");
            selectBands ("0", 0);
        }, "07_linear_phase" });
        snap->jobs.push_back ({ [&p, selectBands]
        {
            p.presets.loadPreset (0);
            for (int b = 0; b < 12; ++b)
                p.addBand ((Shape) (b % 10), 30.0f * std::pow (1.8f, (float) b), (b % 2 ? 5.0f : -4.0f), 1.2f, 3, (Placement) (b % 5));
            selectBands ("3", 3);
        }, "08_many_bands" });
        snap->jobs.push_back ({ [&p, selectBands]
        {
            p.presets.loadPreset (0);
            p.addBand (Shape::lowCut, 30.0f, 0.0f, 0.71f, 8);
            p.addBand (Shape::highCut, 14000.0f, 0.0f, 2.5f, 3);
            p.addBand (Shape::notch, 2400.0f, 0.0f, 6.0f, 1);
            p.addBand (Shape::flatTilt, 1000.0f, 6.0f, 1.0f);
            auto& r = p.param (ids::gainScale); r.setValueNotifyingHost (r.convertTo0to1 (100.0f));
            selectBands ("1", 1);
        }, "09_cuts_and_tilt", 1.0f, [] (StEqEditor& e) { e.getDisplay().setDisplayRange (3); } });
        snap->jobs.push_back ({ [load, selectBands, &p] { load ("Hum Remover 60 Hz"); p.uiState.setProperty ("piano", true, nullptr); selectBands ("0", 0); }, "11_piano" });
        snap->jobs.push_back ({ [&p, selectBands] { p.uiState.setProperty ("piano", false, nullptr); p.presets.loadPreset (0); selectBands ("", -1); }, "10_eq_match", 1.0f,
                                [] (StEqEditor& e) { e.openMatch(); } });

        // instance list: this instance on "Lead Vocal", plus a bass and a guitar track playing
        auto addExtra = [this] (const char* name, juce::uint32 colour, std::function<void (StEqProcessor&)> setup)
        {
            auto e = std::make_unique<StEqProcessor>();
            e->setPlayConfigDetails (2, 2, 48000.0, 800);
            e->prepareToPlay (48000.0, 800);
            juce::AudioProcessor::TrackProperties tp;
            tp.name = juce::String (name);
            tp.colourARGB = colour;
            e->updateTrackProperties (tp);
            setup (*e);
            extras.push_back (std::move (e));
        };
        snap->jobs.push_back ({ [this, &p, load, selectBands, addExtra]
        {
            load ("Vocal Presence");
            selectBands ("", -1);
            juce::AudioProcessor::TrackProperties tp;
            tp.name = juce::String ("Lead Vocal");
            tp.colourARGB = 0xffe0a030;
            p.updateTrackProperties (tp);
            addExtra ("Bass", 0xff3a7ff0, [] (StEqProcessor& e) { e.addBand (Shape::lowCut, 35.0f, 0.0f, 0.71f, 3); e.addBand (Shape::bell, 90.0f, 4.0f, 1.2f); e.addBand (Shape::highCut, 5000.0f, 0.0f, 0.71f, 3); });
            addExtra ("Guitars", 0xff40c070, [] (StEqProcessor& e) { e.addBand (Shape::lowCut, 110.0f, 0.0f, 0.71f, 3); e.addBand (Shape::bell, 2500.0f, 3.0f, 0.9f); e.addBand (Shape::bell, 400.0f, -3.0f, 1.5f); });
           #if JUCE_MAC
            // ST Sat (the installed AU, another binary) on the vocal and guitar tracks
            for (auto* track : { "Lead Vocal", "Guitars" })
            {
                AudioComponentDescription d {};
                d.componentType = kAudioUnitType_Effect; d.componentSubType = 'Stst'; d.componentManufacturer = 'StAu';
                AudioUnit au = nullptr;
                if (auto c = AudioComponentFindNext (nullptr, &d); c != nullptr && AudioComponentInstanceNew (c, &au) == noErr)
                {
                    CFStringRef n = juce::String (track).toCFString();
                    AudioUnitSetProperty (au, kAudioUnitProperty_ContextName, kAudioUnitScope_Global, 0, &n, sizeof (n));
                    CFRelease (n);
                    satUnits.push_back (au);
                }
            }
           #endif
            st::InstanceRegistry::get().flush();
            snap->onTick = [this]
            {
                // the other tracks play too (their spectra and collisions show in the list)
                for (size_t i = 0; i < extras.size(); ++i)
                {
                    juce::AudioBuffer<float> b (2, 800);
                    juce::MidiBuffer m;
                    fillSignal (b, extraPos[i], extraRng[i], extraLp[i]);
                    extras[i]->processBlock (b, m);
                }
            };
        }, "12_instances", 1.0f, [this] (StEqEditor& e)
        {
            e.openInstances();
            if (auto* w = e.getMultiView(); w != nullptr && ! extras.empty())
            {
                w->setHoveredInstance (extras[0]->getInstanceUid());
                if (auto* strip = dynamic_cast<EqStrip*> (w->getModuleComponent (extras[0]->getInstanceUid())))
                {
                    auto& d = strip->getDisplay();
                    d.syncFromParameters();
                    MouseSim m { d, {} };
                    d.mouseMove (m.ev (d.dotPosition (1), {}));
                }
            }
        } });
        snap->jobs.push_back ({ [] {}, "13_output", 1.0f, [] (StEqEditor& e) { e.openOutput(); } });
        snap->jobs.push_back ({ [load, selectBands] { load ("Vocal Presence"); selectBands ("", -1); }, "14_param_display", 1.0f, [] (StEqEditor& e)
        {
            auto& d = e.getDisplay();
            MouseSim m { d, {} };
            d.mouseMove (m.ev (d.dotPosition (2), {}));
            d.mouseMove (m.ev (d.getReadoutPart (2, "gain").getCentre(), {}));
        } });
        snap->startTimerHz (60);
    }
};

juce::JUCEApplicationBase* createSnapshotApp() { return new SnapshotApp(); }
}

int main (int argc, char** argv)
{
    juce::StringArray args (argv + 1, argc - 1);
    if (args.contains ("--snapshot"))
    {
        snapshotDir = juce::File (args[args.indexOf ("--snapshot") + 1]);
        juce::JUCEApplicationBase::createInstance = &createSnapshotApp;
        return juce::JUCEApplicationBase::main (argc, (const char**) argv);
    }
    return runTests();
}
