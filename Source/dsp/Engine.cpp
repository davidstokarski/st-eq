#include "Engine.h"

namespace steq
{

using namespace st::dsp;

PhaseConfig PhaseConfig::make (ProcMode mode, int resolution, double sr)
{
    PhaseConfig c;
    c.mode = mode;
    c.sampleRate = sr;
    const int rate = juce::jmax (1, (int) juce::nextPowerOfTwo (juce::roundToInt (sr / 48000.0)));

    switch (mode)
    {
        case ProcMode::zeroLatency:
            break;

        case ProcMode::naturalPhase:
            c.firDelay = 64 * rate;
            c.firLength = 2 * c.firDelay;
            c.fftSize = 4 * c.firDelay;
            c.partition = c.firDelay;
            c.latency = c.partition + c.firDelay;
            break;

        case ProcMode::linearPhase:
        {
            static constexpr int taps[] = { 4096, 8192, 16384, 32768, 65536 };
            c.firLength = taps[juce::jlimit (0, 4, resolution)] * rate;
            c.firDelay = c.firLength / 2;
            c.fftSize = c.firLength;
            c.partition = juce::jlimit (128, 2048, c.firLength / 32);
            c.latency = c.firDelay + c.partition;
            break;
        }
    }
    return c;
}

Engine::Engine() = default;

void Engine::prepare (double sr, int maxBlockSize, int numChannels, ProcMode mode, int resolution, bool spectralOn)
{
    sampleRate = sr > 0.0 ? sr : 48000.0;
    maxBlock = juce::jmax (1, maxBlockSize);
    numCh = juce::jlimit (1, 2, numChannels);

    dry.setSize (2, maxBlock, false, true, true);
    dryDelayed.setSize (2, maxBlock, false, true, true);
    scDelayed.setSize (2, maxBlock, false, true, true);
    soloBuf.setSize (2, maxBlock, false, true, true);
    bypassBuf.setSize (2, maxBlock, false, true, true);
    bypassMix.prepare (sampleRate, 30.0);
    bypassMix.setCurrentAndTarget (0.0f);
    ms.setSize (2, kSubBlock, false, true, true);

    for (auto& b : bands)
    {
        for (auto& d : b.detector) d.prepare (2);
        b.designDirty = b.detectorDirty = true;
    }

    dcBlocker.prepare (sampleRate, 2, 4.0);
    gainL.prepare (sampleRate, 20.0);
    gainR.prepare (sampleRate, 20.0);
    gainL.setCurrentAndTarget (1.0f);
    gainR.setCurrentAndTarget (1.0f);

    spectral.release();
    setMode (mode, resolution, spectralOn);
    reset();
}

bool Engine::needsModeChange (ProcMode mode, int resolution, bool spectralOn) const noexcept
{
    if (config.mode != mode) return true;
    if (mode == ProcMode::linearPhase && PhaseConfig::make (mode, resolution, sampleRate).firLength != config.firLength) return true;
    if (spectralOn != spectral.isPrepared()) return true;
    return spectralOn && SpectralDynamics::fftOrderFor (resolution, sampleRate) != spectral.getFftOrder();
}

void Engine::setMode (ProcMode mode, int resolution, bool spectralOn)
{
    config = PhaseConfig::make (mode, resolution, sampleRate);
    if (spectralOn) spectral.prepare (sampleRate, SpectralDynamics::fftOrderFor (resolution, sampleRate), numCh);
    else            spectral.release();

    if (mode != ProcMode::zeroLatency)
        convolver.prepare (config.partition, config.firLength, numCh, 4);

    const int delay = juce::jmax (1, config.latency);
    dryDelay.prepare (2, delay);
    dryDelay.setDelay (config.latency);
    scDelay.prepare (2, delay);
    scDelay.setDelay (config.latency);

    for (auto& b : bands) { b.designDirty = true; b.delta.reset(); b.deltaDyn = 1.0e9f; }

    // soft bypass: the dry signal delayed by the full latency
    bypassDelay.prepare (2, juce::jmax (1, getLatencySamples()));
    bypassDelay.setDelay (getLatencySamples());
}

void Engine::reset()
{
    for (auto& b : bands)
    {
        b.filter.reset();
        b.delta.reset();
        for (auto& d : b.detector) d.reset();
        b.env = 0.0f;
        b.dynDb = 0.0f;
        b.avgDb = -40.0f;
    }
    if (convolver.isPrepared()) convolver.reset();
    spectral.reset();
    dryDelay.reset();
    scDelay.reset();
    bypassDelay.reset();
    soloFilterA.reset();
    soloFilterB.reset();
    dcBlocker.reset();
}

//==============================================================================
void Engine::setBand (int index, const BandParams& p) noexcept
{
    auto& b = bands[(size_t) index];
    if (! (p == b.params))
    {
        const auto& o = b.params;
        using juce::exactlyEqual;
        if (o.shape != p.shape || ! exactlyEqual (o.freq, p.freq) || ! exactlyEqual (o.q, p.q) || o.gainQ != p.gainQ || ! exactlyEqual (o.gain, p.gain)
            || o.trigger != p.trigger || ! exactlyEqual (o.trigLo, p.trigLo) || ! exactlyEqual (o.trigHi, p.trigHi)
            || ! exactlyEqual (o.attackMs, p.attackMs) || ! exactlyEqual (o.releaseMs, p.releaseMs))
            b.detectorDirty = true;
        b.designDirty = true;
        b.params = p;
    }
    b.active = p.isActive();
    b.dynamic = b.active && p.isDynamic();
    b.spectral = b.dynamic && p.isSpectral() && spectral.isPrepared();
    if (spectral.isPrepared()) spectral.setBand (index, p, global.gainScale);
}

void Engine::setGlobal (const GlobalSettings& g) noexcept
{
    if (! juce::exactlyEqual (g.gainScale, global.gainScale))
        for (auto& b : bands) b.designDirty = true;
    global = g;
}

void Engine::updateDetector (Band& b) noexcept
{
    const auto& p = b.params;
    b.det0 = b.det1 = false;

    if (p.trigger == Trigger::free)
    {
        b.detector[0].setParameters (BiquadType::highPass, sampleRate, juce::jmin (p.trigLo, p.trigHi), 0.7071);
        b.detector[1].setParameters (BiquadType::lowPass, sampleRate, juce::jmax (p.trigLo, p.trigHi), 0.7071);
        b.det0 = b.det1 = true;
    }
    else
    {
        switch (p.shape)
        {
            case Shape::lowShelf: case Shape::lowCut:
                b.detector[0].setParameters (BiquadType::lowPass, sampleRate, p.freq, 0.7071); b.det0 = true; break;
            case Shape::highShelf: case Shape::highCut: case Shape::tiltShelf:
                b.detector[0].setParameters (BiquadType::highPass, sampleRate, p.freq, 0.7071); b.det0 = true; break;
            case Shape::flatTilt:
                break;
            case Shape::bell: case Shape::notch: case Shape::bandPass: case Shape::allPass:
                b.detector[0].setParameters (BiquadType::bandPass, sampleRate, p.freq, juce::jmax (0.3f, effectiveQ (p, p.gain))); b.det0 = true; break;
        }
    }

    b.attCoeff = onePoleCoeff (p.attackMs, sampleRate);
    b.relCoeff = onePoleCoeff (p.releaseMs, sampleRate);
    b.detectorDirty = false;
}

void Engine::runDetector (int index, const float* const* src, int numSrc, int n) noexcept
{
    auto& b = bands[(size_t) index];
    if (b.detectorDirty) updateDetector (b);

    const auto place = b.params.place;
    const float* L = src[0];
    const float* R = numSrc > 1 ? src[1] : src[0];

    auto filt = [&b] (int ch, float x) noexcept
    {
        if (b.det0) x = b.detector[0].processSample (ch, x);
        if (b.det1) x = b.detector[1].processSample (ch, x);
        return x;
    };

    float env = b.env;
    for (int i = 0; i < n; ++i)
    {
        float a;
        switch (place)
        {
            case Placement::stereo: a = juce::jmax (std::abs (filt (0, L[i])), std::abs (filt (1, R[i]))); break;
            case Placement::left:   a = std::abs (filt (0, L[i])); break;
            case Placement::right:  a = std::abs (filt (0, R[i])); break;
            case Placement::mid:    a = std::abs (filt (0, 0.5f * (L[i] + R[i]))); break;
            case Placement::side:   a = std::abs (filt (0, 0.5f * (L[i] - R[i]))); break;
            default:                a = 0.0f; break;
        }
        env += (a > env ? b.attCoeff : b.relCoeff) * (a - env);
    }
    b.env = env < 1.0e-12f ? 0.0f : env;

    const float level = gainToDb (b.env, -100.0f);
    if (level > -70.0f)
        b.avgDb += (float) (1.0 - std::exp (-(double) n / (sampleRate * 2.5))) * (level - b.avgDb);

    const float thr = b.params.dynAuto ? juce::jlimit (-80.0f, 0.0f, b.avgDb + 4.0f) : b.params.dynThresh;
    const float over = level - thr;
    constexpr float knee = 6.0f;
    const float t = over <= -0.5f * knee ? 0.0f : (over < 0.5f * knee ? (over + 0.5f * knee) * (over + 0.5f * knee) / (2.0f * knee) : over);
    const float amount = 1.0f - std::exp (-t / 8.0f);
    b.dynDb = b.params.dynRange * amount;

    auto& m = meters[(size_t) index];
    m.dynDb.store (b.dynDb, std::memory_order_relaxed);
    m.levelDb.store (level, std::memory_order_relaxed);
    m.thresholdDb.store (thr, std::memory_order_relaxed);
}

void Engine::updateBandDesign (Band& b, bool glide) noexcept
{
    const float dyn = b.dynamic && ! b.spectral ? b.dynDb : 0.0f;
    b.filter.setDesign (eq::designDigital (makeSpec (b.params, global.gainScale, dyn), sampleRate), glide);
    b.designedDyn = dyn;
    b.designDirty = false;
}

void Engine::applyBand (Band& b, EqCascade& cascade, float* const* io, int n) noexcept
{
    const bool stereo = numCh > 1;
    switch (b.params.place)
    {
        case Placement::stereo:
            cascade.process (0, io[0], n);
            if (stereo) cascade.process (1, io[1], n);
            break;
        case Placement::left:
            cascade.process (0, io[0], n);
            break;
        case Placement::right:
            cascade.process (stereo ? 1 : 0, io[stereo ? 1 : 0], n);
            break;
        case Placement::mid:
        case Placement::side:
        {
            if (! stereo)
            {
                if (b.params.place == Placement::mid) cascade.process (0, io[0], n);
                break;
            }
            float* m = ms.getWritePointer (0);
            float* s = ms.getWritePointer (1);
            for (int i = 0; i < n; ++i) encodeMidSide (io[0][i], io[1][i], m[i], s[i]);
            if (b.params.place == Placement::mid) cascade.process (0, m, n);
            else                                  cascade.process (1, s, n);
            for (int i = 0; i < n; ++i) decodeMidSide (m[i], s[i], io[0][i], io[1][i]);
            break;
        }
    }
    cascade.advance();
}

//==============================================================================
void Engine::process (float* const* io, int numChannels, const float* const* sc, int numSc, int n) noexcept
{
    const int nc = juce::jmin (numChannels, numCh, 2);
    if (nc <= 0 || n <= 0) return;
    n = juce::jmin (n, maxBlock);

    // Keep the dry input (for detectors and solo) and its latency-delayed copy.
    for (int c = 0; c < 2; ++c)
    {
        const float* src = io[juce::jmin (c, nc - 1)];
        dry.copyFrom (c, 0, src, n);
        dryDelayed.copyFrom (c, 0, src, n);
        dryDelay.processChannel (c, dryDelayed.getWritePointer (c), n);
    }
    dryDelay.advance (n);

    const bool haveSc = sc != nullptr && numSc > 0;
    if (haveSc)
    {
        for (int c = 0; c < 2; ++c)
        {
            scDelayed.copyFrom (c, 0, sc[juce::jmin (c, numSc - 1)], n);
            scDelay.processChannel (c, scDelayed.getWritePointer (c), n);
        }
        scDelay.advance (n);
    }

    const bool linear = config.mode == ProcMode::linearPhase;
    const float* dryPtr[2] = { linear ? dryDelayed.getReadPointer (0) : dry.getReadPointer (0),
                               linear ? dryDelayed.getReadPointer (1) : dry.getReadPointer (1) };
    const float* scPtr[2] = { nullptr, nullptr };
    if (haveSc)
    {
        scPtr[0] = linear ? scDelayed.getReadPointer (0) : sc[0];
        scPtr[1] = linear ? scDelayed.getReadPointer (1) : sc[juce::jmin (1, numSc - 1)];
    }

    if (linear)
        convolver.process (io, nc, n);

    for (int pos = 0; pos < n; pos += kSubBlock)
    {
        const int k = juce::jmin (kSubBlock, n - pos);
        float* sub[2] = { io[0] + pos, nc > 1 ? io[1] + pos : io[0] + pos };

        for (int bi = 0; bi < kMaxBands; ++bi)
        {
            auto& b = bands[(size_t) bi];
            if (! b.params.used) { b.wasActive = false; continue; }

            if (b.dynamic && ! b.spectral)
            {
                const bool useSc = b.params.sidechain && haveSc;
                const float* src[2] = { (useSc ? scPtr[0] : dryPtr[0]) + pos, (useSc ? scPtr[1] : dryPtr[1]) + pos };
                runDetector (bi, src, 2, k);
            }
            else if (! juce::exactlyEqual (b.dynDb, 0.0f) || b.wasDynamic)
            {
                b.dynDb = 0.0f;
                meters[(size_t) bi].dynDb.store (0.0f, std::memory_order_relaxed);
            }
            b.wasDynamic = b.dynamic;

            if (! b.active) { b.wasActive = false; continue; }

            if (! linear)
            {
                if (! b.wasActive) { b.filter.reset(); updateBandDesign (b, false); }
                else if (b.designDirty || (b.dynamic && std::abs (b.dynDb - b.designedDyn) > 0.05f)) updateBandDesign (b, true);   // 0.05 dB steps, glided
                applyBand (b, b.filter, sub, k);
            }
            else if (b.dynamic && ! b.spectral)
            {
                if (std::abs (b.dynDb - b.deltaDyn) > 0.05f || b.designDirty)
                {
                    BandParams d = b.params;
                    d.gain = 0.0f;
                    b.delta.setDesign (eq::designDigital (makeSpec (d, global.gainScale, b.dynDb), sampleRate), b.wasActive);
                    b.deltaDyn = b.dynDb;
                    b.designDirty = false;
                }
                applyBand (b, b.delta, sub, k);
            }
            b.wasActive = true;
        }
    }

    if (config.mode == ProcMode::naturalPhase)
        convolver.process (io, nc, n);

    const bool soloing = soloBand >= 0 && soloBand < kMaxBands && bands[(size_t) soloBand].params.used;
    if (soloing) processSolo (io, n);
    else         lastSoloBand = -2;

    if (spectral.isPrepared())
    {
        const float* d[2] = { dryDelayed.getReadPointer (0), dryDelayed.getReadPointer (1) };
        const float* s[2] = { scDelayed.getReadPointer (0), scDelayed.getReadPointer (1) };
        spectral.process (io, nc, d, haveSc ? s : nullptr, soloing, n);
    }

    applyCharacter (io, n);

    // Output: gain (+ auto gain), balance (left / right, or mid / side), polarity.
    autoGainSmoothed += 0.2f * (autoGainDb.load (std::memory_order_relaxed) - autoGainSmoothed);
    const float g = dbToGain (global.outGainDb + (global.autoGain ? autoGainSmoothed : 0.0f)) * (global.phaseInvert ? -1.0f : 1.0f);
    gainL.setTarget (g * (global.pan > 0.0f ? 1.0f - global.pan : 1.0f));   // left, or mid
    gainR.setTarget (g * (global.pan < 0.0f ? 1.0f + global.pan : 1.0f));   // right, or side
    if (nc > 1 && global.panMidSide)
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (io[0][i] + io[1][i]) * gainL.getNext();
            const float s = 0.5f * (io[0][i] - io[1][i]) * gainR.getNext();
            io[0][i] = m + s;
            io[1][i] = m - s;
        }
    }
    else if (nc > 1)
    {
        for (int i = 0; i < n; ++i) { io[0][i] *= gainL.getNext(); io[1][i] *= gainR.getNext(); }
    }
    else
    {
        for (int i = 0; i < n; ++i) { io[0][i] *= gainL.getNext(); gainR.getNext(); }
    }

    // Soft bypass, latency compensated (the delay line always runs so switching is click-free).
    for (int c = 0; c < 2; ++c)
    {
        bypassBuf.copyFrom (c, 0, dry, c, 0, n);
        bypassDelay.processChannel (c, bypassBuf.getWritePointer (c), n);
    }
    bypassDelay.advance (n);
    bypassMix.setTarget (global.bypass ? 1.0f : 0.0f);
    if (bypassMix.isSmoothing() || global.bypass)
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = bypassMix.getNext();
            for (int c = 0; c < nc; ++c)
                io[c][i] += m * (bypassBuf.getSample (c, i) - io[c][i]);
        }
    }
}

void Engine::processSolo (float* const* io, int n) noexcept
{
    const auto& b = bands[(size_t) soloBand];
    const auto& p = b.params;

    if (soloBand != lastSoloBand || soloTrigger != lastSoloTrigger || ! (p == soloParams))
    {
        soloHasA = soloHasB = false;
        eq::Digital da, db;
        auto spec = [] (Shape s, float f, float q) { EqBandSpec e; e.shape = s; e.freq = f; e.q = q; e.order = 2; return e; };

        if (soloTrigger && p.trigger == Trigger::free)
        {
            da = eq::designDigital (spec (Shape::lowCut, juce::jmin (p.trigLo, p.trigHi), 0.7071f), sampleRate);
            db = eq::designDigital (spec (Shape::highCut, juce::jmax (p.trigLo, p.trigHi), 0.7071f), sampleRate);
            soloHasA = soloHasB = true;
        }
        else
        {
            switch (p.shape)
            {
                case Shape::lowShelf: case Shape::lowCut:
                    da = eq::designDigital (spec (Shape::highCut, p.freq, 0.7071f), sampleRate); soloHasA = true; break;
                case Shape::highShelf: case Shape::highCut: case Shape::tiltShelf:
                    da = eq::designDigital (spec (Shape::lowCut, p.freq, 0.7071f), sampleRate); soloHasA = true; break;
                case Shape::flatTilt:
                    break;
                case Shape::bell: case Shape::notch: case Shape::bandPass: case Shape::allPass:
                    da = eq::designDigital (spec (Shape::bandPass, p.freq, juce::jmax (0.4f, effectiveQ (p, p.gain))), sampleRate); soloHasA = true; break;
            }
        }

        const bool fresh = soloBand != lastSoloBand || soloTrigger != lastSoloTrigger;
        if (soloHasA) soloFilterA.setDesign (da, ! fresh);
        if (soloHasB) soloFilterB.setDesign (db, ! fresh);
        if (fresh) { soloFilterA.reset(); soloFilterB.reset(); }
        lastSoloBand = soloBand;
        lastSoloTrigger = soloTrigger;
        soloParams = p;
    }

    const bool useSc = soloTrigger && p.sidechain;
    const int nc = juce::jmin (numCh, 2);
    for (int c = 0; c < nc; ++c)
    {
        float* d = io[c];
        juce::FloatVectorOperations::copy (d, (useSc ? scDelayed : dryDelayed).getReadPointer (c), n);
        if (soloHasA) soloFilterA.process (c, d, n);
        if (soloHasB) soloFilterB.process (c, d, n);
    }
    soloFilterA.advance();
    soloFilterB.advance();
}

void Engine::applyCharacter (float* const* io, int n) noexcept
{
    if (global.character == Character::clean) return;

    const bool warm = global.character == Character::warm;
    const float d = warm ? 1.1f : 0.8f;
    const float bias = warm ? 0.12f : 0.0f;
    const float tb = std::tanh (d * bias);
    const float norm = 1.0f / (d * (1.0f - tb * tb));
    const int nc = juce::jmin (numCh, 2);

    for (int c = 0; c < nc; ++c)
    {
        float* x = io[c];
        for (int i = 0; i < n; ++i)
            x[i] = (std::tanh (d * (x[i] + bias)) - tb) * norm;
        if (warm)
            for (int i = 0; i < n; ++i) x[i] = dcBlocker.processSample (c, x[i]);
    }
}

} // namespace steq
