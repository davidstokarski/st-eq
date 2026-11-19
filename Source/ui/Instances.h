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
/** The instance list (bottom bar, centre): covers the whole plugin window, with one row per ST EQ in the session —
    the track's name and colour on the left, its full EQ display (edit any track's bands right there: drag,
    wheel, menus, parameter display, solo) — and a track sidebar on the right that shows which tracks are in
    view and scrolls to one on click. The header has a row-size zoom, a search filter, options, the sidebar
    toggle and close (back to the EQ; Esc too). The row under the mouse grows a little; rows of tracks that collide with
    this one glow red; pinned rows stay at the top. */
class InstanceWindow : public juce::Component, private juce::Timer
{
public:
    InstanceWindow (StEqProcessor& own, InstanceHub&);
    ~InstanceWindow() override;

    /** Called when the user closes the list (the owner deletes it, asynchronously). */
    std::function<void()> onClose;

    int getNumRows() const noexcept { return (int) rows.size(); }
    /** The editable display of an instance's row (null if it has none). */
    EqDisplay* getRowDisplay (st::ProcessorBase*) const;
    /** Forces the magnified row (tests / renders; normally the row under the mouse). */
    void setHoveredInstance (st::ProcessorBase* p) { forcedHover = p; }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    class Row;
    class Sidebar;
    class ZoomSlider;
    class HeaderButton;

private:
    StEqProcessor& own;
    InstanceHub& hub;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Viewport viewport;
    juce::Component holder;
    std::unique_ptr<Sidebar> sidebar;
    std::unique_ptr<ZoomSlider> zoom;
    std::unique_ptr<HeaderButton> menuButton, sidebarButton, closeButton;
    juce::TextEditor search;
    bool showSidebar = true, sortByName = false;
    st::ProcessorBase* forcedHover = nullptr;
    juce::Array<st::ProcessorBase*> pinned;
    int ticks = 0;

    static constexpr int headerHeight = 44, sidebarWidth = 150, nameWidth = 150;
    float baseRowHeight() const;
    void rebuildRows();
    void layoutRows();
    void scrollTo (st::ProcessorBase*);
    void togglePin (st::ProcessorBase*);
    void showMenu();
    void timerCallback() override;
    friend class Row;
    friend class Sidebar;
};

} // namespace steq
