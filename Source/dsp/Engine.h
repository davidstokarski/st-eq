#pragma once
#include "../Params.h"
#include "Spectral.h"

namespace steq
{

struct GlobalSettings
{
    float outGainDb = 0.0f, pan = 0.0f, gainScale = 1.0f;
    bool autoGain = false, phaseInvert = false, bypass = false, panMidSide = false;
    Character character = Character::clean;
};

/** Convolution layout for a processing mode at a sample rate (shared by the engine and the FIR worker). */
struct PhaseConfig
{
    ProcMode mode = ProcMode::zeroLatency;
    double sampleRate = 48000.0;
    int firLength = 0;        // linear phase: FIR taps; natural phase: correction FIR length (2 * delay)
    int firDelay = 0;         // centre of the FIR (samples)
    int fftSize = 0;          // design FFT size used by the worker
    int partition = 0;        // convolver partition (adds this much latency)
    int latency = 0;

    static PhaseConfig make (ProcMode, int resolution, double sampleRate);
};

/** The whole EQ, independent of juce::AudioProcessor so it can be tested offline.

    Signal flow per block (sub-blocks of <= 32 samples for dynamics):
      Zero Latency : bands (analog-matched IIR, in band order, each on its stereo placement) -> character -> output
      Natural Phase: same IIR bands -> short FIR that turns their response into the exact analog one
                     (magnitude and phase, written by FirWorker) -> character -> output
      Linear Phase : one linear-phase FIR of all static band gains (FirWorker) -> dynamic bands as small
                     IIR gain deltas -> character -> output
    3D mode (bands with depth, stereo only): the band's change to the signal (output - input of its filter) is
    moved back: a boost's added part loses some directness and gains width (a decorrelated copy in the side)
    and a few early reflections (a small room); a cut's mid part fades with depth, so at 100 % only the sides
    are cut. Zero latency in every mode: in Linear Phase, depth bands run as IIR bands (left out of the FIR).
    Bands with spectral dynamics contribute their static gain above, and their dynamic part in a
    SpectralDynamics STFT stage that follows (only prepared while such a band exists; adds its FFT size of
    latency).
    Dynamic bands detect on the band-limited input (or external side chain); in Linear Phase the detector
    is delayed by the latency so gain changes line up with the audio. Solo replaces the output with the
    band's region (delay compensated). */
class Engine
{
public:
    static constexpr int kSubBlock = 32;

    Engine();

    /** Message thread. Allocates. Also applies `setMode`. */
    void prepare (double sampleRate, int maxBlockSize, int numChannels, ProcMode, int resolution, bool spectral = false);
    /** Message thread with processing suspended: changes mode / linear-phase resolution / whether the spectral
        dynamics stage runs (reallocates; the latency changes). */
    void setMode (ProcMode, int resolution, bool spectral);
    /** True if a mode change is needed for these settings. */
    bool needsModeChange (ProcMode, int resolution, bool spectral) const noexcept;
    void reset();

    const PhaseConfig& getPhaseConfig() const noexcept { return config; }
    int getLatencySamples() const noexcept { return config.latency + spectral.getLatencySamples(); }
    bool isSpectralActive() const noexcept { return spectral.isPrepared(); }
    SpectralDynamics& getSpectral() noexcept { return spectral; }
    double getSampleRate() const noexcept { return sampleRate; }
    st::dsp::PartitionedConvolver& getConvolver() noexcept { return convolver; }

    /** Audio thread, per block (cheap when nothing changed). */
    void setBand (int band, const BandParams&) noexcept;
    void setGlobal (const GlobalSettings&) noexcept;
    /** Audio thread. band < 0 = off. trigger = hear the dynamics trigger signal instead of the band. */
    void setSolo (int band, bool trigger) noexcept { soloBand = band; soloTrigger = trigger; }

    /** Processes `numSamples` (<= maxBlockSize) in place. Side chain may be null. */
    void process (float* const* io, int numChannels, const float* const* sidechain, int numScChannels, int numSamples) noexcept;

    //==============================================================================
    /** UI feeds (relaxed atomics). Spectral bands report through getSpectral().meters. */
    std::array<BandMeter, kMaxBands> meters;
    const BandMeter& meterFor (int band, const BandParams& p) const noexcept
    {
        return p.isSpectral() && spectral.isPrepared() ? spectral.meters[(size_t) band] : meters[(size_t) band];
    }
    /** Written by the FIR worker (auto gain is computed from the static curve). */
    std::atomic<float> autoGainDb { 0.0f };

private:
    struct Band
    {
        BandParams params;
        bool active = false, dynamic = false, spectral = false, designDirty = true;
        st::dsp::EqCascade filter, delta;
        float designedDyn = 0.0f, deltaDyn = 0.0f;

        // dynamics
        std::array<st::dsp::Biquad, 2> detector;   // [0] band / high pass, [1] low pass (free trigger)
        bool detectorDirty = true, det0 = false, det1 = false, wasActive = false, wasDynamic = false, wasIir = false;
        float env = 0.0f, avgDb = -40.0f, dynDb = 0.0f, attCoeff = 0.1f, relCoeff = 0.01f;

        // depth (3D mode)
        float depthNow = 0.0f;                       // smoothed 0..1
        std::vector<float> roomBuf;                  // early reflections: delay line of the (damped) mid change
        std::array<std::vector<float>, 3> apBuf;     // decorrelator: three short allpasses
        std::array<int, 3> apPos {};
        int roomPos = 0;
        float roomLp = 0.0f;
    };

    struct Tap { int delay; float gl, gr; };
    std::array<Tap, 6> roomTaps {};
    int roomMask = 0;
    float roomLpCoeff = 0.5f, depthStep = 0.001f;

    std::array<Band, kMaxBands> bands;
    GlobalSettings global;
    PhaseConfig config;
    double sampleRate = 48000.0;
    int maxBlock = 0, numCh = 2;
    int soloBand = -1, lastSoloBand = -2;
    bool soloTrigger = false, lastSoloTrigger = false;

    st::dsp::PartitionedConvolver convolver;
    SpectralDynamics spectral;
    st::dsp::DelayLine dryDelay, scDelay;
    juce::AudioBuffer<float> dry, dryDelayed, scDelayed, ms, soloBuf;
    st::dsp::EqCascade soloFilterA, soloFilterB;
    BandParams soloParams;
    bool soloHasA = false, soloHasB = false;
    st::dsp::DcBlocker dcBlocker;
    st::dsp::LinearSmoother gainL, gainR, bypassMix;
    st::dsp::DelayLine bypassDelay;
    juce::AudioBuffer<float> bypassBuf;
    float autoGainSmoothed = 0.0f;

    void updateBandDesign (Band&, bool glide) noexcept;
    void updateDetector (Band&) noexcept;
    void runDetector (int b, const float* const* src, int numSrc, int n) noexcept;
    void applyBand (Band&, st::dsp::EqCascade&, float* const* io, int n) noexcept;
    /** Runs the band, with its depth when it has one (or is fading it out). */
    void applyBandWithDepth (Band&, int index, float* const* io, int n) noexcept;
    void processSolo (float* const* io, int n) noexcept;
    void applyCharacter (float* const* io, int n) noexcept;
};

} // namespace steq
