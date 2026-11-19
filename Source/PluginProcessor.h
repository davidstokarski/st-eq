#pragma once
#include "Params.h"
#include "dsp/Engine.h"
#include "dsp/FirWorker.h"

class StEqProcessor : public st::ProcessorBase,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::AsyncUpdater
{
public:
    StEqProcessor();
    ~StEqProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    double getTailLengthSeconds() const override { return 0.1; }

    //==============================================================================
    steq::ParamRefs refs;
    steq::Engine& getEngine() noexcept { return engine; }
    st::AnalyzerSource scAnalyzer;

    juce::RangedAudioParameter& bandParam (int band, const juce::String& suffix) { return param (steq::ids::band (band, suffix)); }
    steq::BandParams getBand (int b) const noexcept { return refs.readBand (b); }
    int getNumUsedBands() const noexcept;
    /** Live dynamics feed of a band (regular or spectral). UI thread. */
    const steq::BandMeter& bandMeter (int b) const noexcept { return engine.meterFor (b, refs.readBand (b)); }

    /** Message thread, undoable. Returns the new band's slot or -1 if all 24 are used. */
    int addBand (steq::Shape, float freq, float gain, float q, int slope = 1, steq::Placement = steq::Placement::stereo, bool startNewTransaction = true);
    void removeBand (int band);
    /** Sets a band parameter (plain value) as a complete gesture. */
    void setBandValue (int band, const juce::String& suffix, float plain);
    void setValue (const juce::String& id, float plain);

    /** Copy / paste bands as XML text (clipboard format). */
    juce::String bandsToText (const juce::Array<int>& bands) const;
    juce::Array<int> pasteBands (const juce::String& text, bool startNewTransaction = true);

    /** Solo (listen) a band: band < 0 = off. Not saved. */
    std::atomic<int> soloBand { -1 };
    std::atomic<bool> soloTrigger { false };

    /** Applies a pending processing-mode / resolution change now (normally async on the message thread). */
    void applyModeChange();
    /** Waits for the FIR worker to finish (tests / offline): computes the FIR synchronously. */
    void refreshFirNow();

    bool hasSidechain() const;
    /** True if any used band has spectral dynamics switched on (the STFT stage then runs). */
    bool anySpectralBand() const noexcept;

private:
    steq::Engine engine;
    steq::FirWorker worker;
    bool prepared = false;
    double currentRate = 48000.0;
    int currentChannels = 2;

    static juce::StringArray modeParameterIds();
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { applyModeChange(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StEqProcessor)
};
