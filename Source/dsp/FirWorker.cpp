#include "FirWorker.h"

namespace steq
{

using Cpx = std::complex<float>;

FirWorker::FirWorker (const ParamRefs& r, Engine& e) : juce::Thread ("ST EQ FIR"), refs (r), engine (e) {}

FirWorker::~FirWorker() { stop(); }

bool FirWorker::sameFilter (const BandParams& a, const BandParams& b) noexcept
{
    using juce::exactlyEqual;
    return a.used == b.used && a.bypass == b.bypass && a.shape == b.shape && exactlyEqual (a.freq, b.freq) && exactlyEqual (a.gain, b.gain)
        && exactlyEqual (a.q, b.q) && a.slope == b.slope && a.place == b.place && a.gainQ == b.gainQ;
}

void FirWorker::start()
{
    stop();
    config = engine.getPhaseConfig();
    cacheValid.fill (false);
    lastValid = false;
    numBins = 0;
    mags.clear();
    ratios.clear();

    if (config.mode != ProcMode::zeroLatency)
    {
        numBins = config.fftSize / 2 + 1;
        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) config.fftSize)));
        work.assign ((size_t) (2 * config.fftSize), 0.0f);
        for (auto& g : groups) g.assign ((size_t) numBins, Cpx {});
        for (auto& ir : irs) ir.assign ((size_t) config.firLength, 0.0f);

        // Window exactly symmetric about the FIR centre D (keeps linear phase exact).
        window.resize ((size_t) config.firLength);
        const bool blackman = config.mode == ProcMode::linearPhase;
        for (int n = 0; n < config.firLength; ++n)
        {
            const double x = juce::MathConstants<double>::pi * (double) (n - config.firDelay) / (double) config.firDelay;
            window[(size_t) n] = (float) (blackman ? 0.42 + 0.5 * std::cos (x) + 0.08 * std::cos (2.0 * x) : 0.5 + 0.5 * std::cos (x));
        }

        if (config.mode == ProcMode::linearPhase)
            mags.assign ((size_t) kMaxBands, std::vector<float> ((size_t) numBins, 1.0f));
        else
            ratios.assign ((size_t) kMaxBands, std::vector<Cpx> ((size_t) numBins, Cpx (1.0f, 0.0f)));
    }

    update (true);
    startThread (juce::Thread::Priority::low);
}

void FirWorker::stop()
{
    stopThread (2000);
}

void FirWorker::run()
{
    while (! threadShouldExit())
    {
        update (false);
        wait (12);
    }
}

bool FirWorker::update (bool force)
{
    const float scale = refs.readGainScale();
    for (int b = 0; b < kMaxBands; ++b)
        current[(size_t) b] = refs.readBand (b);

    bool changed = force || ! lastValid || ! juce::exactlyEqual (scale, lastScale);
    for (int b = 0; b < kMaxBands && ! changed; ++b)
        changed = ! sameFilter (current[(size_t) b], lastSnapshot[(size_t) b]);
    if (! changed) return false;

    lastSnapshot = current;
    lastScale = scale;
    lastValid = true;

    computeAutoGain (scale);
    if (config.mode == ProcMode::linearPhase)  computeLinear (scale);
    if (config.mode == ProcMode::naturalPhase) computeNatural (scale);
    return true;
}

void FirWorker::computeAutoGain (float scale)
{
    // Weighted average of the static curve over the audible range (flat 50 Hz..10 kHz, tapering outside).
    double sum = 0.0, wsum = 0.0;
    constexpr int points = 48;
    std::array<st::dsp::eq::Analog, kMaxBands> designs;
    std::array<float, kMaxBands> weightFor {};
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = current[(size_t) b];
        if (! p.isActive()) continue;
        designs[(size_t) b] = st::dsp::eq::designAnalog (makeSpec (p, scale));
        weightFor[(size_t) b] = p.place == Placement::stereo ? 1.0f : 0.5f;
    }

    for (int i = 0; i < points; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, (double) i / (points - 1));
        double db = 0.0;
        for (int b = 0; b < kMaxBands; ++b)
            if (weightFor[(size_t) b] > 0.0f)
                db += weightFor[(size_t) b] * designs[(size_t) b].magnitudeDb (f);
        const double w = f < 50.0 ? std::pow (f / 50.0, 1.0) : (f > 10000.0 ? 10000.0 / f : 1.0);
        sum += w * juce::jlimit (-60.0, 60.0, db);
        wsum += w;
    }
    engine.autoGainDb.store ((float) juce::jlimit (-30.0, 30.0, -sum / juce::jmax (1.0e-9, wsum)));
}

void FirWorker::computeLinear (float scale)
{
    const double fs = config.sampleRate;
    const int N = config.fftSize;

    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = current[(size_t) b];
        if (! p.isActive()) continue;
        if (cacheValid[(size_t) b] && juce::exactlyEqual (cachedScale, scale) && sameFilter (p, cachedFor[(size_t) b])) continue;

        const auto a = st::dsp::eq::designAnalog (makeSpec (p, scale));
        auto& m = mags[(size_t) b];
        for (int k = 0; k < numBins; ++k)
            m[(size_t) k] = (float) a.magnitude (juce::jmax (0.5, (double) k * fs / N));
        cachedFor[(size_t) b] = p;
        cacheValid[(size_t) b] = true;
        if (isThreadRunning() && threadShouldExit()) return;
    }
    cachedScale = scale;

    for (auto& g : groups) std::fill (g.begin(), g.end(), Cpx (1.0f, 0.0f));
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = current[(size_t) b];
        if (! p.isActive()) continue;
        auto& g = groups[(size_t) p.place];
        const auto& m = mags[(size_t) b];
        for (int k = 0; k < numBins; ++k) g[(size_t) k] *= m[(size_t) k];
    }
    writeMatrix (false);
}

void FirWorker::computeNatural (float scale)
{
    const double fs = config.sampleRate;
    const int M = config.fftSize;

    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = current[(size_t) b];
        if (! p.isActive()) continue;
        if (cacheValid[(size_t) b] && juce::exactlyEqual (cachedScale, scale) && sameFilter (p, cachedFor[(size_t) b])) continue;

        const auto a = st::dsp::eq::designAnalog (makeSpec (p, scale));
        const auto d = st::dsp::eq::designDigital (a, fs);
        auto& r = ratios[(size_t) b];
        for (int k = 0; k < numBins; ++k)
        {
            const double f = juce::jmin ((double) k * fs / M, 0.4999 * fs);
            const auto ha = a.response (juce::jmax (0.5, f));
            const auto hd = d.response (juce::jmax (0.5, f), fs);
            std::complex<double> e (1.0, 0.0);
            if (std::abs (hd) > 1.0e-5)
            {
                e = ha / hd;
                const double mag = std::abs (e);
                if (mag > 4.0 || mag < 0.25) e *= juce::jlimit (0.25, 4.0, mag) / mag;   // never correct more than 12 dB
            }
            r[(size_t) k] = Cpx ((float) e.real(), (float) e.imag());
        }
        cachedFor[(size_t) b] = p;
        cacheValid[(size_t) b] = true;
    }
    cachedScale = scale;

    for (auto& g : groups) std::fill (g.begin(), g.end(), Cpx (1.0f, 0.0f));
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = current[(size_t) b];
        if (! p.isActive()) continue;
        auto& g = groups[(size_t) p.place];
        const auto& r = ratios[(size_t) b];
        for (int k = 0; k < numBins; ++k) g[(size_t) k] *= r[(size_t) k];
    }
    writeMatrix (true);
}

void FirWorker::writeMatrix (bool natural)
{
    bool hasLR = false, hasMS = false;
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = current[(size_t) b];
        if (! p.isActive()) continue;
        hasLR |= p.place == Placement::left || p.place == Placement::right;
        hasMS |= p.place == Placement::mid || p.place == Placement::side;
    }

    using Routing = st::dsp::PartitionedConvolver::Routing;
    const Routing routing = hasMS ? Routing::matrix : (hasLR ? Routing::diagonal : Routing::mono);
    const int count = routing == Routing::mono ? 1 : (routing == Routing::diagonal ? 2 : 4);
    const int N = config.fftSize, L = config.firLength, D = config.firDelay;

    const auto& gs = groups[0]; const auto& gl = groups[1]; const auto& gr = groups[2];
    const auto& gm = groups[3]; const auto& gS = groups[4];

    for (int e = 0; e < count; ++e)
    {
        // element spectrum
        for (int k = 0; k < numBins; ++k)
        {
            Cpx v;
            const auto K = (size_t) k;
            if (routing == Routing::mono)          v = gs[K];
            else if (routing == Routing::diagonal) v = gs[K] * (e == 0 ? gl[K] : gr[K]);
            else
            {
                const Cpx sum = 0.5f * (gm[K] + gS[K]), diff = 0.5f * (gm[K] - gS[K]);
                switch (e)
                {
                    case 0:  v = gs[K] * gl[K] * sum;  break;   // L <- L
                    case 1:  v = gs[K] * gl[K] * diff; break;   // L <- R
                    case 2:  v = gs[K] * gr[K] * diff; break;   // R <- L
                    default: v = gs[K] * gr[K] * sum;  break;   // R <- R
                }
            }

            if (natural)   // put the FIR centre at D
                v *= std::polar (1.0f, (float) (-juce::MathConstants<double>::twoPi * k * D / N));

            work[(size_t) (2 * k)] = v.real();
            work[(size_t) (2 * k + 1)] = v.imag();
        }
        for (int k = 1; k < N / 2; ++k)
        {
            work[(size_t) (2 * (N - k))] = work[(size_t) (2 * k)];
            work[(size_t) (2 * (N - k) + 1)] = -work[(size_t) (2 * k + 1)];
        }
        fft->performRealOnlyInverseTransform (work.data());

        auto& ir = irs[(size_t) e];
        for (int n = 0; n < L; ++n)
        {
            // linear: zero-phase response rotated so its centre sits at D; natural: already delayed by D
            const int src = natural ? n : (n - D + N) % N;
            ir[(size_t) n] = work[(size_t) src] * window[(size_t) n];
        }
    }

    const float* ptrs[4] = { irs[0].data(), irs[1].data(), irs[2].data(), irs[3].data() };
    engine.getConvolver().writeImpulseResponses (routing, ptrs, L);
}

} // namespace steq
