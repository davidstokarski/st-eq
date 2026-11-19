#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryPresets.h"

using namespace steq;

namespace
{
    constexpr int kChunk = 64;   // parameter update granularity
}

StEqProcessor::StEqProcessor()
    : ProcessorBase (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                      .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false),
                     createParameterLayout()),
      worker (refs, engine)
{
    refs.bind (apvts);
    registerFactoryPresets (presets);

    // ST EQ identity: a neutral, slightly violet charcoal display (instead of ST Sat's red haze), a saturated
    // yellow response curve and a light-grey analyser, like a modern Pro-style EQ.
    auto& theme = st::Theme::get();
    theme.displayTop  = juce::Colour (0xff27252c);
    theme.displayGlow = juce::Colour (0xff3b3a43);
    theme.bg0 = juce::Colour (0xff111013);
    theme.bg1 = juce::Colour (0xff18171b);
    theme.bg2 = juce::Colour (0xff1f1e23);
    theme.bg3 = juce::Colour (0xff29282e);
    theme.bg4 = juce::Colour (0xff36353c);
    theme.stroke = juce::Colour (0xff36353b);
    theme.strokeSoft = juce::Colour (0xff201f24);
    theme.glassPanel = juce::Colour (0xa8292830);
    theme.spectrum = juce::Colour (0xffd9d7e0);
    theme.curve = juce::Colour (0xfff5d240);
    st::Knob::setDoubleClickEntersValue (true);   // Pro-style knobs: double-click types a value, Cmd-click resets
    for (auto& id : modeParameterIds()) apvts.addParameterListener (id, this);
}

juce::StringArray StEqProcessor::modeParameterIds()
{
    // Anything that changes the latency / processing layout: mode, resolution, and whether any band uses
    // spectral dynamics (the STFT stage only runs while one exists).
    juce::StringArray a { ids::mode, ids::resolution };
    for (int b = 0; b < kMaxBands; ++b) { a.add (ids::band (b, "spectral")); a.add (ids::band (b, "used")); }
    return a;
}

bool StEqProcessor::anySpectralBand() const noexcept
{
    for (auto& b : refs.bands)
        if (b.used.getBool() && b.spectral.getBool()) return true;
    return false;
}

StEqProcessor::~StEqProcessor()
{
    unregisterInstance();   // first: views of this instance (elsewhere) still use its members
    for (auto& id : modeParameterIds()) apvts.removeParameterListener (id, this);
    cancelPendingUpdate();
    worker.stop();
}

bool StEqProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (in != out) return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

bool StEqProcessor::hasSidechain() const
{
    if (auto* bus = getBus (true, 1))
        return bus->isEnabled() && bus->getNumberOfChannels() > 0;
    return false;
}

void StEqProcessor::prepareToPlay (double sampleRate, int)
{
    prepareShell (sampleRate);
    scAnalyzer.prepare (sampleRate);

    currentRate = sampleRate;
    currentChannels = juce::jlimit (1, 2, getMainBusNumOutputChannels());

    worker.stop();
    engine.prepare (sampleRate, kChunk, currentChannels, (ProcMode) juce::jlimit (0, 2, refs.mode.getInt()), refs.resolution.getInt(), anySpectralBand());
    worker.start();
    prepared = true;
    setLatencySamples (engine.getLatencySamples());
}

void StEqProcessor::releaseResources()
{
    worker.stop();
}

void StEqProcessor::applyModeChange()
{
    if (! prepared) return;
    const auto mode = (ProcMode) juce::jlimit (0, 2, refs.mode.getInt());
    const int res = juce::jlimit (0, 4, refs.resolution.getInt());
    const bool spectral = anySpectralBand();
    if (! engine.needsModeChange (mode, res, spectral))
        return;

    // Re-preparing the convolver allocates: pause processing (the wrapper outputs silence for the
    // few blocks this takes), rebuild, restart the FIR worker, then tell the host the new latency.
    worker.stop();
    suspendProcessing (true);
    engine.setMode (mode, res, spectral);
    engine.reset();
    suspendProcessing (false);
    worker.start();
    setLatencySamples (engine.getLatencySamples());
}

void StEqProcessor::refreshFirNow()
{
    worker.stop();
    worker.computeNow();
}

//==============================================================================
void StEqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, false, 0);
    const int total = buffer.getNumSamples();
    const int numIn = getMainBusNumInputChannels();
    for (int c = numIn; c < main.getNumChannels(); ++c)
        main.clear (c, 0, total);

    const int channels = juce::jmin (2, main.getNumChannels());
    if (channels == 0) return;

    juce::AudioBuffer<float> sc;
    const bool scOn = hasSidechain();
    if (scOn) sc = getBusBuffer (buffer, true, 1);
    const int scChannels = scOn ? juce::jmin (2, sc.getNumChannels()) : 0;

    inputMeter.push (main, total);
    preAnalyzer.push (main, total);
    if (scChannels > 0) scAnalyzer.push (sc, total);

    GlobalSettings g;
    g.outGainDb = refs.outGain.get();
    g.pan = refs.outPan.get() * 0.01f;
    g.autoGain = refs.autoGain.getBool();
    g.gainScale = refs.readGainScale();
    g.phaseInvert = refs.phaseInv.getBool();
    g.character = (Character) juce::jlimit (0, 2, refs.character.getInt());
    g.bypass = refs.bypass.getBool();
    g.panMidSide = refs.panMode.getInt() == 1;

    for (int start = 0; start < total; start += kChunk)
    {
        const int n = juce::jmin (kChunk, total - start);
        for (int b = 0; b < kMaxBands; ++b)
            engine.setBand (b, refs.readBand (b));
        engine.setGlobal (g);
        engine.setSolo (soloBand.load (std::memory_order_relaxed), soloTrigger.load (std::memory_order_relaxed));

        float* io[2] = { main.getWritePointer (0, start), channels > 1 ? main.getWritePointer (1, start) : nullptr };
        const float* scp[2] = { scChannels > 0 ? sc.getReadPointer (0, start) : nullptr,
                                scChannels > 1 ? sc.getReadPointer (1, start) : nullptr };
        engine.process (io, channels, scChannels > 0 ? scp : nullptr, scChannels, n);
    }

    outputMeter.push (main, total);
    postAnalyzer.push (main, total);
}

//==============================================================================
int StEqProcessor::getNumUsedBands() const noexcept
{
    int n = 0;
    for (int b = 0; b < kMaxBands; ++b) n += refs.bands[(size_t) b].used.getBool() ? 1 : 0;
    return n;
}

void StEqProcessor::setValue (const juce::String& id, float plain)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
        p->endChangeGesture();
    }
}

void StEqProcessor::setBandValue (int band, const juce::String& suffix, float plain)
{
    setValue (ids::band (band, suffix), plain);
}

int StEqProcessor::addBand (Shape shape, float freq, float gain, float q, int slope, Placement place, bool newTransaction)
{
    int slot = -1;
    for (int b = 0; b < kMaxBands; ++b)
        if (! refs.bands[(size_t) b].used.getBool()) { slot = b; break; }
    if (slot < 0) return -1;

    if (newTransaction) undoManager.beginNewTransaction ("Add band");
    for (auto& s : ids::bandSuffixes)
    {
        auto& p = bandParam (slot, s);
        p.setValueNotifyingHost (p.getDefaultValue());
    }
    setBandValue (slot, "shape", (float) (int) shape);
    setBandValue (slot, "freq", freq);
    setBandValue (slot, "gain", gain);
    setBandValue (slot, "q", q);
    setBandValue (slot, "slope", (float) slope);
    setBandValue (slot, "place", (float) (int) place);
    setBandValue (slot, "used", 1.0f);
    return slot;
}

void StEqProcessor::removeBand (int band)
{
    if (! juce::isPositiveAndBelow (band, kMaxBands)) return;
    if (soloBand.load() == band) soloBand = -1;
    for (auto& s : ids::bandSuffixes)
    {
        auto& p = bandParam (band, s);
        p.beginChangeGesture();
        p.setValueNotifyingHost (p.getDefaultValue());
        p.endChangeGesture();
    }
}

juce::String StEqProcessor::bandsToText (const juce::Array<int>& bands) const
{
    juce::XmlElement root ("STEQBands");
    for (int b : bands)
    {
        if (! juce::isPositiveAndBelow (b, kMaxBands)) continue;
        auto* e = root.createNewChildElement ("Band");
        for (auto& s : ids::bandSuffixes)
            if (s != "used")
                e->setAttribute (s, (double) apvts.getRawParameterValue (ids::band (b, s))->load());
    }
    return root.toString (juce::XmlElement::TextFormat().singleLine());
}

juce::Array<int> StEqProcessor::pasteBands (const juce::String& text, bool newTransaction)
{
    juce::Array<int> added;
    auto xml = juce::parseXML (text);
    if (xml == nullptr || ! xml->hasTagName ("STEQBands")) return added;

    if (newTransaction) undoManager.beginNewTransaction ("Paste bands");
    for (auto* e : xml->getChildWithTagNameIterator ("Band"))
    {
        const int slot = addBand ((Shape) e->getIntAttribute ("shape"), (float) e->getDoubleAttribute ("freq", 1000.0),
                                  (float) e->getDoubleAttribute ("gain"), (float) e->getDoubleAttribute ("q", 1.0), 1, Placement::stereo, false);
        if (slot < 0) break;
        for (auto& s : ids::bandSuffixes)
            if (s != "used" && e->hasAttribute (s))
                setBandValue (slot, s, (float) e->getDoubleAttribute (s));
        added.add (slot);
    }
    return added;
}

std::unique_ptr<juce::Component> StEqProcessor::createStripView()
{
    return std::make_unique<steq::EqStrip> (*this);
}

juce::AudioProcessorEditor* StEqProcessor::createEditor()
{
    return new StEqEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new StEqProcessor();
}
