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
    void instanceRemoved (st::ProcessorBase*) override;
};

//==============================================================================
/** Instance list (bottom bar, centre): every ST EQ in the session with its track colour and name, its EQ
    curve over its spectrum, a red glow when it collides with this instance, a toggle for taking part in
    the collision display, and inline editing of its EQ (expand a row). */
class InstancePanel : public st::Popover, private juce::Timer
{
public:
    InstancePanel (StEqProcessor& own, InstanceHub&, EqDisplay& ownDisplay);
    ~InstancePanel() override;

    static constexpr int panelWidth = 760;
    int getPanelHeight() const;
    /** Called when rows are added / removed / expanded (the owner re-positions the popover). */
    std::function<void()> onHeightChanged;

    /** Opens the inline editor of `instance` (null closes it). */
    void expandInstance (st::ProcessorBase* instance) { if (instance != expanded) toggleExpanded (instance == nullptr ? expanded : instance); }
    bool hasInlineEditor() const;

    class Row;

protected:
    void layoutContent (juce::Rectangle<int>) override;
    void paintContent (juce::Graphics&) override;

private:
    StEqProcessor& own;
    InstanceHub& hub;
    EqDisplay& ownDisplay;
    juce::Viewport viewport;
    juce::Component holder;
    std::vector<std::unique_ptr<Row>> rows;
    st::ProcessorBase* expanded = nullptr;

    void rebuildRows();
    void toggleExpanded (st::ProcessorBase*);
    int rowsHeight() const;
    void timerCallback() override;
};

} // namespace steq
