#pragma once
#include "../PluginProcessor.h"
#include "EqDisplay.h"
#include "BandIsland.h"
#include "Instances.h"

namespace steq
{

//==============================================================================
/** EQ Match: learns the long-term spectrum of the side-chain reference and of the input, then
    creates bands that make the input match the reference (fitted like EQ Sketch). */
class MatchPopover : public st::Popover, private juce::Timer
{
public:
    MatchPopover (StEqProcessor&, EqDisplay&, InstanceHub&);
    ~MatchPopover() override;
    static constexpr int panelWidth = 660, panelHeight = 240;
    static constexpr int width = panelWidth + 2 * margin, height = panelHeight + 2 * margin;

protected:
    void layoutContent (juce::Rectangle<int>) override;
    void paintContent (juce::Graphics&) override;

private:
    StEqProcessor& proc;
    EqDisplay& display;
    InstanceHub& hub;
    AnalyzerSettings savedAnalyzer;
    st::PillButton learn { "Learn" }, clear { "Clear" }, apply { "Apply" };
    st::FlatButton bandsButton { "", "Maximum number of bands to create" };
    st::FlatButton sourceButton { "", "Reference: the side-chain input, or another ST EQ instance in the session" };
    st::FlatButton detailButton { "", "Detail: how finely the match follows the difference (smoothing)" };
    st::FlatButton amountButton { "", "Amount of the match to apply" };
    juce::WeakReference<st::ProcessorBase> reference;   // null = side chain
    int maxBands = 8, detailIndex = 2, amountPct = 100;
    static constexpr float detailOctaves[5] = { 1.0f, 0.5f, 1.0f / 3.0f, 1.0f / 6.0f, 1.0f / 12.0f };
    void updateButtons();
    const std::vector<float>* referenceLevels() const;
    bool learning = false;
    int frames = 0;
    std::vector<double> curSum, refSum;
    std::vector<float> target;   // smoothed difference per display point (dB)
    juce::Rectangle<float> graph;

    void timerCallback() override;
    void computeTarget();
    void applyMatch();
};

//==============================================================================
/** Output settings (bottom bar, right): output gain with the pan / balance as its outer ring, pan mode
    (left / right or mid / side), gain scale, auto gain and polarity. */
class OutputPanel : public st::Popover
{
public:
    explicit OutputPanel (StEqProcessor&);
    static constexpr int panelWidth = 330, panelHeight = 196;
    static constexpr int width = panelWidth + 2 * margin, height = panelHeight + 2 * margin;

protected:
    void layoutContent (juce::Rectangle<int>) override;
    void paintContent (juce::Graphics&) override;

private:
    st::Knob output, scale;
    st::Dropdown panMode;
    st::ToggleChip autoGain, phase;
};

//==============================================================================
/** Bottom bar (Pro-style): processing mode / resolution on the left, the instance list in the centre,
    analyser, character, global bypass and output on the right. */
class FooterBar : public juce::Component, private juce::Timer
{
public:
    explicit FooterBar (StEqProcessor&);
    ~FooterBar() override;

    std::function<void (const AnalyzerSettings&)> onAnalyzerChanged;
    std::function<void()> onMatchRequested, onInstancesRequested, onOutputRequested;
    void setAnalyzer (const AnalyzerSettings&);
    juce::Rectangle<int> getInstancesButtonBounds() const { return instancesButton.getBounds(); }
    juce::Rectangle<int> getOutputButtonBounds() const { return outputButton.getBounds(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    StEqProcessor& proc;
    st::Dropdown mode, resolution, character;
    st::FlatButton analyzerButton { "Analyzer", "Spectrum analyser settings" };
    st::FlatButton instancesButton { "", "Instance list: every ST EQ in the session, with collisions and inline editing" };
    st::FlatButton outputButton { "", "Output: gain, pan, gain scale, auto gain, polarity" };
    st::IconButton bypass { st::icons::Icon::power, "Bypass the whole EQ (soft, latency compensated)" };
    juce::ButtonParameterAttachment bypassAttachment;
    AnalyzerSettings analyzer;
    std::vector<float> dividers;
    bool lastLinear = false;
    juce::String lastInstances, lastOutput;

    void timerCallback() override;
    void showAnalyzerMenu();
};

} // namespace steq
