#include "Sketch.h"

namespace steq::sketch
{

namespace
{
    float bandDb (const FitBand& b, float hz)
    {
        st::dsp::EqBandSpec s;
        s.shape = b.shape; s.freq = b.freq; s.gainDb = b.gain; s.q = b.q; s.order = slopeToOrder (b.slope);
        return (float) st::dsp::eq::designAnalog (s).magnitudeDb (hz);
    }

    struct Problem
    {
        std::vector<float> f, t;   // grid and target
        float fLo = 20.0f, fHi = 20000.0f;

        float sse (const std::vector<float>& model, const FitBand* extra, std::vector<float>* extraOut = nullptr) const
        {
            float e = 0.0f;
            for (size_t i = 0; i < f.size(); ++i)
            {
                float m = model[i];
                if (extra != nullptr)
                {
                    const float d = bandDb (*extra, f[i]);
                    if (extraOut != nullptr) (*extraOut)[i] = d;
                    m += d;
                }
                const float r = t[i] - m;
                e += r * r;
            }
            return e;
        }
    };

    /** Coordinate descent on (log f, gain, log q) of one band against model + band. */
    float refine (const Problem& pb, const std::vector<float>& model, FitBand& b, int iterations)
    {
        float best = pb.sse (model, &b);
        float stepF = 0.25f, stepG = 1.5f, stepQ = 0.35f;   // octaves, dB, log2

        for (int it = 0; it < iterations; ++it)
        {
            bool improved = false;
            for (int param = 0; param < 3; ++param)
            {
                for (float dir : { 1.0f, -1.0f })
                {
                    FitBand c = b;
                    if (param == 0) c.freq = juce::jlimit (pb.fLo * 0.5f, pb.fHi * 2.0f, b.freq * std::exp2 (dir * stepF));
                    if (param == 1) c.gain = juce::jlimit (-30.0f, 30.0f, b.gain + dir * stepG);
                    if (param == 2) c.q = juce::jlimit (0.1f, 18.0f, b.q * std::exp2 (dir * stepQ));
                    const float e = pb.sse (model, &c);
                    if (e < best) { best = e; b = c; improved = true; break; }
                }
            }
            if (! improved)
            {
                stepF *= 0.5f; stepG *= 0.5f; stepQ *= 0.5f;
                if (stepG < 0.02f) break;
            }
        }
        return best;
    }
}

float response (const std::vector<FitBand>& bands, float hz)
{
    float db = 0.0f;
    for (auto& b : bands) db += bandDb (b, hz);
    return db;
}

std::vector<FitBand> fit (const std::vector<std::pair<float, float>>& target, int maxBands, float toleranceDb)
{
    std::vector<FitBand> bands;
    if (target.size() < 2) return bands;

    Problem pb;
    pb.fLo = juce::jmax (10.0f, target.front().first);
    pb.fHi = juce::jmin (30000.0f, target.back().first);
    if (pb.fHi <= pb.fLo * 1.05f) return bands;

    constexpr int n = 96;
    size_t j = 0;
    for (int i = 0; i < n; ++i)
    {
        const float hz = pb.fLo * std::pow (pb.fHi / pb.fLo, (float) i / (n - 1));
        while (j + 1 < target.size() - 1 && target[j + 1].first < hz) ++j;
        const auto& a = target[j];
        const auto& b = target[juce::jmin (j + 1, target.size() - 1)];
        const float span = std::log (b.first / juce::jmax (1.0f, a.first));
        const float tt = span > 1.0e-6f ? juce::jlimit (0.0f, 1.0f, std::log (hz / a.first) / span) : 0.0f;
        pb.f.push_back (hz);
        pb.t.push_back (a.second + (b.second - a.second) * tt);
    }

    std::vector<float> model ((size_t) n, 0.0f);
    auto rebuildModel = [&]
    {
        std::fill (model.begin(), model.end(), 0.0f);
        for (auto& b : bands)
            for (int i = 0; i < n; ++i) model[(size_t) i] += bandDb (b, pb.f[(size_t) i]);
    };

    for (int iter = 0; iter < maxBands; ++iter)
    {
        // residual
        int kMax = 0;
        float rMax = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float r = pb.t[(size_t) i] - model[(size_t) i];
            if (std::abs (r) > std::abs (rMax)) { rMax = r; kMax = i; }
        }
        if (std::abs (rMax) < toleranceDb) break;

        std::vector<FitBand> candidates;

        // bell at the largest residual, width from the half-gain points
        {
            int lo = kMax, hi = kMax;
            auto res = [&] (int i) { return pb.t[(size_t) i] - model[(size_t) i]; };
            while (lo > 0 && res (lo - 1) * rMax > 0.0f && std::abs (res (lo - 1)) > 0.5f * std::abs (rMax)) --lo;
            while (hi < n - 1 && res (hi + 1) * rMax > 0.0f && std::abs (res (hi + 1)) > 0.5f * std::abs (rMax)) ++hi;
            const float octaves = juce::jmax (0.1f, std::log2 (pb.f[(size_t) hi] / pb.f[(size_t) lo]));
            const float bw = std::exp2 (octaves);
            FitBand b;
            b.shape = Shape::bell;
            b.freq = pb.f[(size_t) kMax];
            b.gain = rMax;
            b.q = juce::jlimit (0.2f, 12.0f, std::sqrt (bw) / (bw - 1.0f));
            candidates.push_back (b);
        }

        // shelves at the ends of the drawn span
        for (int side = 0; side < 2; ++side)
        {
            const int edge = side == 0 ? 0 : n - 1;
            const float r0 = pb.t[(size_t) edge] - model[(size_t) edge];
            if (std::abs (r0) < 1.0f) continue;
            int k = edge;
            const int step = side == 0 ? 1 : -1;
            while (k + step >= 0 && k + step < n && std::abs (pb.t[(size_t) (k + step)] - model[(size_t) (k + step)]) > 0.5f * std::abs (r0)) k += step;
            FitBand b;
            b.shape = side == 0 ? Shape::lowShelf : Shape::highShelf;
            b.freq = pb.f[(size_t) k];
            b.gain = r0;
            b.q = 0.7071f;
            candidates.push_back (b);
        }

        float bestErr = pb.sse (model, nullptr);
        FitBand best;
        bool found = false;
        for (auto c : candidates)
        {
            const float e = refine (pb, model, c, 40);
            if (e < bestErr * 0.98f) { bestErr = e; best = c; found = true; }
        }
        if (! found) break;
        bands.push_back (best);
        rebuildModel();
    }

    // joint refinement: each band against all the others
    for (int pass = 0; pass < 3; ++pass)
        for (size_t k = 0; k < bands.size(); ++k)
        {
            std::vector<float> others ((size_t) n, 0.0f);
            for (size_t o = 0; o < bands.size(); ++o)
                if (o != k)
                    for (int i = 0; i < n; ++i) others[(size_t) i] += bandDb (bands[o], pb.f[(size_t) i]);
            refine (pb, others, bands[k], 25);
        }

    bands.erase (std::remove_if (bands.begin(), bands.end(), [] (const FitBand& b) { return std::abs (b.gain) < 0.25f; }), bands.end());
    for (auto& b : bands)
        b.gain = std::round (b.gain * 10.0f) / 10.0f;
    return bands;
}

} // namespace steq::sketch
