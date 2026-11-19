#pragma once
#include "../Params.h"

namespace steq
{

/** Per-band UI feed (relaxed atomics). */
struct BandMeter
{
    std::atomic<float> dynDb { 0.0f }, levelDb { -100.0f }, thresholdDb { -30.0f };
};

/** Spectral dynamics: for every band with spectral dynamics on, the dynamic part of the band (the difference
    between its range curve and its static curve) is applied per FFT bin, each bin compressed / expanded from
    its own level. That tames resonances that come and go inside a wide band without touching the rest.

    Runs in series after the main EQ (which applies the bands' static gains) as one STFT stage
    (linear phase, latency = FFT size). Detection uses the band's input (latency-aligned dry signal) or the
    external side chain, with
      - Density : frequency smoothing of the detector (low = groups of bins react together, high = per bin),
      - Tilt    : +3 dB/oct detector tilt around the band frequency so bright content isn't under-detected,
      - Auto threshold: long-term average detector level of the band + 6 dB.
    A free trigger range uses the loudest bin in that range for the whole band. */
class SpectralDynamics
{
public:
    static constexpr int kGrid = 64;   // display grid (log 20 Hz .. 20 kHz)
    static float gridFrequency (int i) noexcept { return 20.0f * std::pow (1000.0f, (float) i / (float) (kGrid - 1)); }
    /** FFT order for a linear-phase resolution index at a sample rate. */
    static int fftOrderFor (int resolution, double sampleRate) noexcept;

    /** Message thread (allocates). */
    void prepare (double sampleRate, int fftOrder, int numChannels);
    void release() { stft.release(); order = 0; }
    void reset() noexcept;
    bool isPrepared() const noexcept { return stft.isPrepared(); }
    int getLatencySamples() const noexcept { return stft.isPrepared() ? stft.getLatencySamples() : 0; }
    int getFftOrder() const noexcept { return order; }

    /** Audio thread. */
    void setBand (int band, const BandParams&, float gainScale) noexcept;
    /** dry / sc: two latency-aligned detector channels each (sc may be null). bypassGains keeps detecting but
        leaves the audio untouched (solo). */
    void process (float* const* io, int numChannels, const float* const* dry, const float* const* sc,
                  bool bypassGains, int numSamples) noexcept;

    /** UI feeds. amount: 0..1 of the band's dynamic range applied around each grid frequency. */
    std::array<BandMeter, kMaxBands> meters;
    std::array<std::array<std::atomic<float>, kGrid>, kMaxBands> amount {};

private:
    struct Band
    {
        BandParams params;
        float scale = 1.0f;
        bool on = false, dirty = true, wasOn = false;
        std::vector<float> delta, offset, coef, env;   // per bin: dB delta at full range, detector offset (tilt), smoothing coef, envelope (dB)
        int k0 = 0, k1 = -1;                          // bins with a dynamic delta
        int t0 = 0, t1 = -1;                          // free trigger bins
        float avgDb = -60.0f, att = 0.5f, rel = 0.05f, maxDelta = 0.0f;
    };

    st::dsp::StftProcessor stft;
    double sampleRate = 48000.0;
    int order = 0, numCh = 2, bins = 0;
    std::array<Band, kMaxBands> bands;
    std::vector<float> binHz;
    std::array<std::vector<float>, 10> power;      // [source * 5 + placement]
    std::array<bool, 10> powerReady {};
    std::array<std::vector<float>, 5> gainDb;      // per placement
    std::array<int, 5> gainLo {}, gainHi {};
    std::vector<float> smooth;
    std::array<int, kGrid> gridBin {};
    bool bypass = false, haveSc = false;

    void design (Band&) noexcept;
    const float* detectorPower (int source, Placement) noexcept;
    void frame() noexcept;
};

} // namespace steq
