#include "Spectral.h"

namespace steq
{

namespace
{
    constexpr float kKnee = 6.0f;
    constexpr int kDesignPoints = 384;   // log grid the band delta is evaluated on (then interpolated per bin)

    float mapAmount (float over) noexcept
    {
        const float t = over <= -0.5f * kKnee ? 0.0f
                      : (over < 0.5f * kKnee ? (over + 0.5f * kKnee) * (over + 0.5f * kKnee) / (2.0f * kKnee) : over);
        return 1.0f - std::exp (-t / 8.0f);
    }
}

int SpectralDynamics::fftOrderFor (int resolution, double sr) noexcept
{
    const int rate = juce::jmax (1, (int) juce::nextPowerOfTwo (juce::roundToInt (sr / 48000.0)));
    int o = 10 + juce::jlimit (0, 2, resolution);   // Low 1024, Medium 2048, High (and above) 4096 at 44.1 / 48 kHz
    for (int r = rate; r > 1; r >>= 1) ++o;
    return juce::jmin (o, 14);
}

void SpectralDynamics::prepare (double sr, int fftOrder, int channels)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    order = fftOrder;
    numCh = juce::jlimit (1, 2, channels);
    stft.prepare (fftOrder, numCh, 4);
    bins = stft.getNumBins();

    binHz.resize ((size_t) bins);
    for (int k = 0; k < bins; ++k) binHz[(size_t) k] = (float) (k * sampleRate / stft.getFftSize());
    for (auto& p : power) p.assign ((size_t) bins, 0.0f);
    for (auto& g : gainDb) g.assign ((size_t) bins, 0.0f);
    smooth.assign ((size_t) bins, 0.0f);
    for (auto& b : bands)
    {
        b.delta.assign ((size_t) bins, 0.0f);
        b.offset.assign ((size_t) bins, 0.0f);
        b.coef.assign ((size_t) bins, 0.0f);
        b.env.assign ((size_t) bins, -100.0f);
        b.dirty = true;
    }
    for (int i = 0; i < kGrid; ++i)
        gridBin[(size_t) i] = juce::jlimit (0, bins - 1, juce::roundToInt (gridFrequency (i) * (double) stft.getFftSize() / sampleRate));
    reset();
}

void SpectralDynamics::reset() noexcept
{
    if (! stft.isPrepared()) return;
    stft.reset();
    for (auto& b : bands)
    {
        std::fill (b.env.begin(), b.env.end(), -100.0f);
        b.avgDb = -60.0f;
    }
}

void SpectralDynamics::setBand (int index, const BandParams& p, float gainScale) noexcept
{
    auto& b = bands[(size_t) index];
    if (! (p == b.params) || ! juce::exactlyEqual (gainScale, b.scale))
    {
        b.params = p;
        b.scale = gainScale;
        b.dirty = true;
    }
    b.on = p.isSpectral() && p.isActive();
}

void SpectralDynamics::design (Band& b) noexcept
{
    const auto& p = b.params;
    const auto a0 = st::dsp::eq::designAnalog (makeSpec (p, b.scale, 0.0f));
    const auto a1 = st::dsp::eq::designAnalog (makeSpec (p, b.scale, p.dynRange));

    // Delta on a log grid, interpolated per bin.
    std::array<float, kDesignPoints> grid {};
    const double lo = 5.0, hi = juce::jmax (lo * 2.0, sampleRate * 0.5);
    const double step = std::log (hi / lo) / (kDesignPoints - 1);
    for (int i = 0; i < kDesignPoints; ++i)
    {
        const double f = lo * std::exp (step * i);
        grid[(size_t) i] = (float) (a1.magnitudeDb (f) - a0.magnitudeDb (f));
    }

    b.maxDelta = 0.0f;
    for (int k = 0; k < bins; ++k)
    {
        const double f = juce::jmax (lo, (double) binHz[(size_t) k]);
        const double x = std::log (f / lo) / step;
        const int i = juce::jlimit (0, kDesignPoints - 2, (int) x);
        const float t = (float) juce::jlimit (0.0, 1.0, x - i);
        const float d = grid[(size_t) i] + t * (grid[(size_t) i + 1] - grid[(size_t) i]);
        b.delta[(size_t) k] = d;
        b.maxDelta = juce::jmax (b.maxDelta, std::abs (d));
    }
    const float floor = juce::jmax (0.02f, b.maxDelta * 0.01f);
    b.k0 = bins; b.k1 = -1;
    for (int k = 1; k < bins; ++k)
        if (std::abs (b.delta[(size_t) k]) > floor) { b.k0 = juce::jmin (b.k0, k); b.k1 = k; }

    // Detector: tilt around the band frequency, frequency smoothing from the density.
    const float wOct = (1.0f / 3.0f) * std::pow (1.0f - juce::jlimit (0.0f, 100.0f, p.specDensity) * 0.01f, 1.5f);
    const float binWidth = (float) (sampleRate / stft.getFftSize());
    for (int k = 0; k < bins; ++k)
    {
        const float f = juce::jmax (binWidth, binHz[(size_t) k]);
        b.offset[(size_t) k] = p.specTilt ? 3.0f * std::log2 (f / juce::jmax (20.0f, p.freq)) : 0.0f;
        const float tau = juce::jmax (0.25f, wOct * f * 0.6931f / binWidth);
        b.coef[(size_t) k] = std::exp (-1.0f / tau);
    }

    b.t0 = 0; b.t1 = -1;
    if (p.trigger == Trigger::free)
    {
        b.t0 = juce::jlimit (1, bins - 1, (int) (juce::jmin (p.trigLo, p.trigHi) / binWidth));
        b.t1 = juce::jlimit (1, bins - 1, (int) std::ceil (juce::jmax (p.trigLo, p.trigHi) / binWidth));
    }

    const double hopSec = (stft.getHopSize()) / sampleRate;
    b.att = (float) (1.0 - std::exp (-hopSec / juce::jmax (1.0e-4, p.attackMs * 0.001)));
    b.rel = (float) (1.0 - std::exp (-hopSec / juce::jmax (1.0e-3, p.releaseMs * 0.001)));
    b.dirty = false;
}

const float* SpectralDynamics::detectorPower (int source, Placement place) noexcept
{
    const int idx = source * 5 + (int) place;
    auto& out = power[(size_t) idx];
    if (powerReady[(size_t) idx]) return out.data();

    const auto* L = stft.getSideSpectrum (source * 2);
    const auto* R = stft.getSideSpectrum (source * 2 + 1);
    const float norm = stft.getSineNormalisation() * stft.getSineNormalisation();
    for (int k = 0; k < bins; ++k)
    {
        float pw;
        switch (place)
        {
            case Placement::stereo: pw = juce::jmax (std::norm (L[k]), std::norm (R[k])); break;
            case Placement::left:   pw = std::norm (L[k]); break;
            case Placement::right:  pw = std::norm (R[k]); break;
            case Placement::mid:    pw = std::norm (0.5f * (L[k] + R[k])); break;
            case Placement::side:   pw = std::norm (0.5f * (L[k] - R[k])); break;
            default:                pw = 0.0f; break;
        }
        out[(size_t) k] = pw * norm;
    }
    powerReady[(size_t) idx] = true;
    return out.data();
}

void SpectralDynamics::process (float* const* io, int nc, const float* const* dry, const float* const* sc,
                                bool bypassGains, int n) noexcept
{
    if (! stft.isPrepared()) return;
    bypass = bypassGains;
    haveSc = sc != nullptr;
    const float* side[4] = { dry[0], dry[1], sc != nullptr ? sc[0] : nullptr, sc != nullptr ? sc[1] : nullptr };
    stft.process (io, nc, side, 4, n, [this] (st::dsp::StftProcessor&) { frame(); });

    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& band = bands[(size_t) b];
        if (! band.on && band.wasOn)
        {
            band.wasOn = false;
            meters[(size_t) b].dynDb.store (0.0f, std::memory_order_relaxed);
            for (auto& a : amount[(size_t) b]) a.store (0.0f, std::memory_order_relaxed);
        }
    }
}

void SpectralDynamics::frame() noexcept
{
    powerReady.fill (false);
    for (int p = 0; p < 5; ++p) { gainLo[(size_t) p] = bins; gainHi[(size_t) p] = -1; }

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        auto& b = bands[(size_t) bi];
        if (! b.on) continue;
        if (b.dirty) design (b);
        if (! b.wasOn) { std::fill (b.env.begin(), b.env.end(), -100.0f); b.wasOn = true; }
        if (b.k1 < b.k0) continue;

        const auto& p = b.params;
        const auto place = numCh < 2 && p.place != Placement::stereo ? Placement::stereo : p.place;
        const float* pw = detectorPower (p.sidechain && haveSc ? 1 : 0, place);

        // region to detect over (band region, or the free trigger range), with smoothing margin
        const bool free = p.trigger == Trigger::free && b.t1 >= b.t0;
        const int d0 = free ? b.t0 : b.k0, d1 = free ? b.t1 : b.k1;
        const int margin = 8;
        const int s0 = juce::jmax (1, d0 - margin), s1 = juce::jmin (bins - 1, d1 + margin);

        // frequency smoothing (forward + backward one-pole across bins), then dB + tilt, then envelope
        float y = pw[s0];
        for (int k = s0; k <= s1; ++k) { const float c = b.coef[(size_t) k]; y = pw[k] + c * (y - pw[k]); smooth[(size_t) k] = y; }
        y = smooth[(size_t) s1];
        for (int k = s1; k >= s0; --k) { const float c = b.coef[(size_t) k]; y = smooth[(size_t) k] + c * (y - smooth[(size_t) k]); smooth[(size_t) k] = y; }

        float levelMax = -100.0f, weightSum = 0.0f, weighted = 0.0f;
        for (int k = d0; k <= d1; ++k)
        {
            const float lv = 10.0f * std::log10 (smooth[(size_t) k] + 1.0e-12f) + b.offset[(size_t) k];
            float& e = b.env[(size_t) k];
            e += (lv > e ? b.att : b.rel) * (lv - e);
            const float w = free ? 1.0f : std::abs (b.delta[(size_t) k]);
            weightSum += w;
            weighted += w * juce::jmax (-100.0f, e);
            if (e > levelMax && (free || std::abs (b.delta[(size_t) k]) > 0.5f * b.maxDelta)) levelMax = e;
        }

        // auto threshold: long-term average band level + 6 dB
        const float mean = weightSum > 0.0f ? weighted / weightSum : -100.0f;
        if (mean > -90.0f)
        {
            const double hopSec = stft.getHopSize() / sampleRate;
            b.avgDb += (float) (1.0 - std::exp (-hopSec / 2.5)) * (mean - b.avgDb);
        }
        const float thr = p.dynAuto ? juce::jlimit (-80.0f, 0.0f, b.avgDb + 6.0f) : p.dynThresh;

        auto& g = gainDb[(size_t) place];
        auto& lo = gainLo[(size_t) place];
        auto& hi = gainHi[(size_t) place];
        if (lo > hi) { std::fill (g.begin(), g.end(), 0.0f); }
        lo = juce::jmin (lo, b.k0);
        hi = juce::jmax (hi, b.k1);

        const float freeAmount = free ? mapAmount (levelMax - thr) : 0.0f;
        float peak = 0.0f;
        for (int k = b.k0; k <= b.k1; ++k)
        {
            const float a = free ? freeAmount : mapAmount (b.env[(size_t) k] - thr);
            const float d = b.delta[(size_t) k] * a;
            g[(size_t) k] += d;
            if (std::abs (d) > std::abs (peak)) peak = d;
        }

        auto& m = meters[(size_t) bi];
        m.dynDb.store (peak, std::memory_order_relaxed);
        m.levelDb.store (levelMax, std::memory_order_relaxed);
        m.thresholdDb.store (thr, std::memory_order_relaxed);
        for (int i = 0; i < kGrid; ++i)
        {
            const int k = gridBin[(size_t) i];
            float a = 0.0f;
            if (k >= b.k0 && k <= b.k1)
                a = free ? freeAmount : mapAmount (b.env[(size_t) k] - thr);
            amount[(size_t) bi][(size_t) i].store (a, std::memory_order_relaxed);
        }
    }

    if (bypass) return;

    // Apply: stereo / left / right per channel, then mid / side.
    auto* L = stft.getSpectrum (0);
    auto* R = numCh > 1 ? stft.getSpectrum (1) : nullptr;
    auto toGain = [] (float db) { return std::pow (10.0f, db * 0.05f); };
    const int st = (int) Placement::stereo, pl = (int) Placement::left, pr = (int) Placement::right;
    const int lo = juce::jmin (gainLo[(size_t) st], gainLo[(size_t) pl], gainLo[(size_t) pr]);
    const int hi = juce::jmax (gainHi[(size_t) st], gainHi[(size_t) pl], gainHi[(size_t) pr]);
    auto gAt = [this] (int p, int k) { return gainLo[(size_t) p] <= gainHi[(size_t) p] && k >= gainLo[(size_t) p] && k <= gainHi[(size_t) p] ? gainDb[(size_t) p][(size_t) k] : 0.0f; };
    for (int k = lo; k <= hi; ++k)
    {
        const float s = gAt (st, k);
        L[k] *= toGain (s + gAt (pl, k));
        if (R != nullptr) R[k] *= toGain (s + gAt (pr, k));
    }

    if (R != nullptr)
    {
        const int pm = (int) Placement::mid, ps = (int) Placement::side;
        const int lo2 = juce::jmin (gainLo[(size_t) pm], gainLo[(size_t) ps]);
        const int hi2 = juce::jmax (gainHi[(size_t) pm], gainHi[(size_t) ps]);
        for (int k = lo2; k <= hi2; ++k)
        {
            const auto m = 0.5f * (L[k] + R[k]) * toGain (gAt (pm, k));
            const auto sd = 0.5f * (L[k] - R[k]) * toGain (gAt (ps, k));
            L[k] = m + sd;
            R[k] = m - sd;
        }
    }
}

} // namespace steq
