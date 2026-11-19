#pragma once
#include "Engine.h"

namespace steq
{

/** Background thread that turns the band parameters into what the audio thread can't afford to compute:
      - Linear Phase: a zero-phase FIR of the exact analog magnitude of all bands (2x2 matrix when bands
        use left/right or mid/side placement), windowed and written to the engine's convolver.
      - Natural Phase: a short correction FIR = analog response / digital IIR response (magnitude and
        phase), so IIR + correction behaves like the analog prototype right up to Nyquist.
      - Auto Gain: static compensation from the EQ curve (all modes).
    It polls the parameters (atomics) every ~12 ms and only recomputes bands that changed. */
class FirWorker : private juce::Thread
{
public:
    FirWorker (const ParamRefs&, Engine&);
    ~FirWorker() override;

    /** Message thread: (re)configures from the engine's current PhaseConfig and starts the thread. */
    void start();
    void stop();

    /** Synchronous update (tests / offline rendering). Call with the thread stopped. */
    void computeNow() { update (true); }

private:
    const ParamRefs& refs;
    Engine& engine;
    PhaseConfig config;

    std::array<BandParams, kMaxBands> current {}, cachedFor {};
    std::array<bool, kMaxBands> cacheValid {};
    float cachedScale = -1.0e9f;
    std::array<BandParams, kMaxBands> lastSnapshot {};
    float lastScale = -1.0e9f;
    bool lastValid = false;

    int numBins = 0;
    std::vector<std::vector<float>> mags;                          // linear: per band |H| per bin
    std::vector<std::vector<std::complex<float>>> ratios;          // natural: per band Ha/Hd per bin
    std::array<std::vector<std::complex<float>>, 5> groups;        // st, L, R, M, S
    std::array<std::vector<float>, 4> irs;
    std::vector<float> work, window;
    std::unique_ptr<juce::dsp::FFT> fft;

    void run() override;
    bool update (bool force);
    void computeAutoGain (float scale);
    void computeLinear (float scale);
    void computeNatural (float scale);
    void writeMatrix (bool complexDelay);

    static bool sameFilter (const BandParams&, const BandParams&) noexcept;
};

} // namespace steq
