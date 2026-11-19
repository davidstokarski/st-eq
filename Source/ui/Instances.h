#pragma once
#include "EqDisplay.h"

namespace steq
{

//==============================================================================
/** The other ST EQ instances in this session (st::InstanceRegistry): a spectrum per instance (read from its
    analyser tap without disturbing it), the collision reference (loudest of the instances taking part, per
    analyser point) and how strongly each one collides with this instance. Message thread. */
class InstanceHub : private st::InstanceRegistry::Listener
{
public:
    explicit InstanceHub (StEqProcessor& own);
    ~InstanceHub() override;

    struct Entry
    {
        StEqProcessor* proc = nullptr;
        juce::WeakReference<st::ProcessorBase> ref;
        std::unique_ptr<st::SpectrumAnalyzer> spectrum;
        bool collide = true;      // part of the collision reference
        float collision = 0.0f;   // 0..1, smoothed peak collision with this instance
    };

    const std::vector<std::unique_ptr<Entry>>& getOthers() const noexcept { return others; }
    int getNumInstances() const noexcept { return (int) others.size() + 1; }

    /** Analyser mapping must match the display's (tilt, points). */
    void setAnalyzer (const AnalyzerSettings&);
    /** Updates spectra and collisions (only while `active`: panel open or collisions shown). */
    void update (double dt, const st::SpectrumAnalyzer& ownPost, bool active);
    /** How strongly instance `p` collides with this one (0 for this instance or unknown ones). */
    float collisionFor (const st::ProcessorBase& p) const;
    /** Collision reference for the display, or null when no other instance takes part. */
    const std::vector<float>* getReference() const noexcept { return hasReference ? &reference : nullptr; }

    /** Called synchronously when the list changes (rows may reference an instance that is going away). */
    std::function<void()> onListChanged;

private:
    StEqProcessor& own;
    std::vector<std::unique_ptr<Entry>> others;
    std::vector<float> reference;
    bool hasReference = false;
    st::SpectrumAnalyzer::Options options;

    void rebuild();
    void instancesChanged() override { rebuild(); }
    void instanceRemoved (uint64_t uid, st::ProcessorBase* local) override;
};

//==============================================================================
/** ST EQ's module in the multiview (st::MultiView, via StEqProcessor::createStripView): the instance's full
    EQ display in compact form (bands, menus, parameter display, solo) and a bypass switch. Also shown by
    other ST plug-ins' multiviews, rendered off screen, so its controls handle the mouse themselves. */
class EqStrip : public juce::Component
{
public:
    explicit EqStrip (StEqProcessor&);
    EqDisplay& getDisplay() noexcept { return display; }
    juce::Component& getBypassSwitch() noexcept { return power; }
    void resized() override;

private:
    /** Bypass switch (top left): lights when the EQ is bypassed; click toggles. */
    struct Power : juce::Component, juce::SettableTooltipClient
    {
        explicit Power (StEqProcessor&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        juce::ParameterAttachment attachment;
        bool on = false;
    };
    EqDisplay display;
    Power power;
};

} // namespace steq
