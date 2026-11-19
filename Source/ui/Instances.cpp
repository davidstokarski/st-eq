#include "Instances.h"

namespace steq
{

namespace
{
    juce::Colour instanceColour (const st::ProcessorBase& p)
    {
        const auto c = p.getTrackColour();
        return c.isTransparent() ? bandColour (p.getInstanceNumber() - 1) : c.withAlpha (1.0f);
    }
}

//==============================================================================
InstanceHub::InstanceHub (StEqProcessor& p) : own (p)
{
    setAnalyzer (AnalyzerSettings {});
    st::InstanceRegistry::get().addListener (this);
    rebuild();
}

InstanceHub::~InstanceHub()
{
    st::InstanceRegistry::get().removeListener (this);
}

void InstanceHub::setAnalyzer (const AnalyzerSettings& a)
{
    // same display mapping as EqDisplay's analysers, lighter FFT (several instances may be read)
    options = {};
    options.fftOrder = 12;
    options.hopSize = 1024;
    options.numDisplayPoints = 400;
    options.minHz = 8.0f;
    options.maxHz = 32000.0f;
    options.slopeDbPerOct = AnalyzerSettings::tilts[a.tiltIndex];
    options.releaseDbPerSec = 24.0f;
    options.attack = 0.5f;
    options.smoothingPasses = 4;
    options.nonConsuming = true;
    for (auto& e : others)
        e->spectrum = std::make_unique<st::SpectrumAnalyzer> (e->proc->postAnalyzer, options);
}

void InstanceHub::rebuild()
{
    const auto all = st::InstanceRegistry::get().getInstances();
    bool changed = false;

    // drop instances that are gone
    for (size_t i = others.size(); i-- > 0;)
        if (! all.contains (others[i]->proc) || others[i]->ref.get() == nullptr)
        {
            others.erase (others.begin() + (long) i);
            changed = true;
        }

    // add new ones (in creation order)
    for (auto* p : all)
    {
        auto* eq = dynamic_cast<StEqProcessor*> (p);
        if (eq == nullptr || eq == &own) continue;
        const bool known = std::any_of (others.begin(), others.end(), [eq] (const auto& e) { return e->proc == eq; });
        if (known) continue;
        auto e = std::make_unique<Entry>();
        e->proc = eq;
        e->ref = p;
        e->spectrum = std::make_unique<st::SpectrumAnalyzer> (eq->postAnalyzer, options);
        others.push_back (std::move (e));
        changed = true;
    }
    std::sort (others.begin(), others.end(), [] (const auto& a, const auto& b) { return a->proc->getInstanceNumber() < b->proc->getInstanceNumber(); });

    // names / colours may have changed too
    juce::ignoreUnused (changed);
    if (onListChanged) onListChanged();
}

void InstanceHub::instanceRemoved (st::ProcessorBase* p)
{
    for (size_t i = 0; i < others.size(); ++i)
        if (others[i]->proc == p)
        {
            others.erase (others.begin() + (long) i);
            break;
        }
    if (onListChanged) onListChanged();
}

void InstanceHub::update (double dt, const st::SpectrumAnalyzer& ownPost, bool active)
{
    hasReference = false;
    if (! active || others.empty()) return;

    const auto& mine = ownPost.getLevelsDb();
    reference.assign (mine.size(), -200.0f);
    for (auto& e : others)
    {
        if (e->ref.get() == nullptr || e->spectrum == nullptr) continue;
        e->spectrum->update (dt);
        const auto& lv = e->spectrum->getLevelsDb();
        const size_t n = juce::jmin (lv.size(), mine.size());

        float peak = 0.0f;
        for (size_t i = 0; i < n; ++i)
            peak = juce::jmax (peak, collisionAmount (mine[i], lv[i]));
        const float k = peak > e->collision ? 0.5f : (float) (1.0 - std::exp (-dt / 0.6));
        e->collision += k * (peak - e->collision);

        if (! e->collide) continue;
        for (size_t i = 0; i < n; ++i) reference[i] = juce::jmax (reference[i], lv[i]);
        hasReference = true;
    }
}

//==============================================================================
class InstancePanel::Row : public juce::Component
{
public:
    static constexpr int collapsedHeight = 50, editorHeight = 210;

    Row (InstancePanel& o, StEqProcessor& p, InstanceHub::Entry* e, const st::SpectrumAnalyzer* ownSpectrum)
        : owner (o), proc (p), entry (e), mySpectrum (ownSpectrum)
    {
        const bool isOwn = entry == nullptr;
        if (! isOwn)
        {
            collide.setClickingTogglesState (true);
            collide.setToggleState (entry->collide, juce::dontSendNotification);
            collide.setOnColour (st::Theme::get().danger);
            collide.onClick = [this] { entry->collide = collide.getToggleState(); };
            addAndMakeVisible (collide);

            expand.setClickingTogglesState (false);
            expand.onClick = [this] { owner.toggleExpanded (&proc); };
            addAndMakeVisible (expand);
        }
        for (auto* b : { &collide, &expand }) b->setMinimal (true);
        setRepaintsOnMouseActivity (true);
        refresh();
    }

    st::ProcessorBase* getProcessor() const noexcept { return &proc; }
    bool isExpanded() const noexcept { return editor != nullptr; }
    int getPreferredHeight() const noexcept { return collapsedHeight + (editor != nullptr ? editorHeight : 0); }

    void setExpanded (bool e)
    {
        if (e == isExpanded() || entry == nullptr) return;
        if (e)
        {
            editor = std::make_unique<EqDisplay> (proc, true);
            addAndMakeVisible (*editor);
        }
        else editor.reset();
        expand.setIcon (e ? st::icons::Icon::chevronUp : st::icons::Icon::chevronDown);
        resized();
    }

    /** Re-reads the instance's bands (cheap when unchanged). */
    void refresh()
    {
        bool changed = ! juce::exactlyEqual (lastScale, proc.refs.readGainScale());
        int used = 0;
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto p = proc.getBand (b);
            used += p.used ? 1 : 0;
            if (! (p == lastBands[(size_t) b])) { lastBands[(size_t) b] = p; changed = true; }
        }
        numBands = used;
        if (! changed && ! curve.empty()) return;
        lastScale = proc.refs.readGainScale();

        curve.assign (kCurvePoints, 0.0f);
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto& p = lastBands[(size_t) b];
            if (! p.isActive()) continue;
            const auto a = st::dsp::eq::designAnalog (makeSpec (p, lastScale));
            for (int i = 0; i < kCurvePoints; ++i)
                curve[(size_t) i] += (float) a.magnitudeDb (curveHz (i));
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (collapsedHeight).reduced (10, 8);
        expand.setBounds (top.removeFromRight (26).withSizeKeepingCentre (24, 24));
        collide.setBounds (top.removeFromRight (30).withSizeKeepingCentre (24, 24));
        top.removeFromRight (8);
        mini = top.removeFromRight (juce::jmin (380, top.getWidth() - 200)).toFloat();
        if (editor != nullptr) editor->setBounds (r.reduced (10, 0).withTrimmedBottom (10));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = st::Theme::get();
        const bool isOwn = entry == nullptr;
        auto r = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
        const auto col = instanceColour (proc);

        // card: own instance slightly lighter; collisions glow red at the edge
        g.setColour ((isOwn ? t.bg3 : t.bg2).withAlpha (isMouseOver (true) ? 0.95f : 0.8f));
        g.fillRoundedRectangle (r, 6.0f);
        const float hit = isOwn ? 0.0f : entry->collision;
        if (hit > 0.02f)
        {
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (t.danger.withAlpha (0.12f * hit / (float) i));
                g.drawRoundedRectangle (r.reduced (-0.5f + (float) i), 6.0f, (float) i * 1.6f);
            }
            g.setColour (t.danger.withAlpha (0.65f * hit));
            g.drawRoundedRectangle (r, 6.0f, 1.0f);
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (isOwn ? 0.10f : 0.05f));
            g.drawRoundedRectangle (r, 6.0f, 1.0f);
        }

        // colour stripe + name
        auto head = r.withHeight ((float) collapsedHeight - 4.0f);
        g.setColour (col);
        g.fillRoundedRectangle (head.removeFromLeft (4.0f).reduced (0.0f, 9.0f).translated (6.0f, 0.0f), 2.0f);
        auto text = head.withTrimmedLeft (18.0f).withRight (mini.getX() - 8.0f);
        g.setFont (st::fonts::semibold (13.5f));
        g.setColour (t.text);
        g.drawText (proc.getInstanceLabel(), text.removeFromTop (text.getHeight() * 0.55f), juce::Justification::bottomLeft, true);
        g.setFont (st::fonts::regular (11.0f));
        g.setColour (t.textDim);
        juce::String sub = isOwn ? "This instance" : modeNames[juce::jlimit (0, 2, proc.refs.mode.getInt())];
        sub << "  ·  " << numBands << (numBands == 1 ? " band" : " bands");
        if (! isOwn && hit > 0.3f) sub << "  ·  collides";
        g.drawText (sub, text, juce::Justification::topLeft, true);

        paintMini (g);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (entry != nullptr && e.mouseWasClicked() && e.y < collapsedHeight) owner.toggleExpanded (&proc);
    }

private:
    static constexpr int kCurvePoints = 96;
    static float curveHz (int i) { return 20.0f * std::pow (1000.0f, (float) i / (float) (kCurvePoints - 1)); }

    InstancePanel& owner;
    StEqProcessor& proc;
    InstanceHub::Entry* entry;
    const st::SpectrumAnalyzer* mySpectrum;
    st::IconButton collide { st::icons::Icon::stereoLink, "Compare with this instance: collisions are shown in red in the analyser" };
    st::IconButton expand { st::icons::Icon::chevronDown, "Edit this instance's EQ here" };
    std::unique_ptr<EqDisplay> editor;
    std::array<BandParams, kMaxBands> lastBands {};
    float lastScale = -1000.0f;
    int numBands = 0;
    std::vector<float> curve;
    juce::Rectangle<float> mini;

    void paintMini (juce::Graphics& g)
    {
        const auto& t = st::Theme::get();
        auto area = mini;
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRoundedRectangle (area, 4.0f);
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());

        auto xFor = [&] (float hz) { return area.getX() + area.getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); };

        // spectrum (grey area), from the instance's analyser
        const auto* sp = entry != nullptr ? entry->spectrum.get() : mySpectrum;
        if (sp != nullptr && sp->getNumPoints() > 1)
        {
            const auto& lv = sp->getLevelsDb();
            juce::Path p;
            p.startNewSubPath (area.getX(), area.getBottom());
            for (int i = 0; i < sp->getNumPoints(); ++i)
            {
                const float hz = sp->getFrequencyForPoint (i);
                if (hz < 20.0f || hz > 20000.0f) continue;
                const float y = area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, -lv[(size_t) i] / 90.0f);
                p.lineTo (xFor (hz), y);
            }
            p.lineTo (area.getRight(), area.getBottom());
            p.closeSubPath();
            g.setColour (t.spectrum.withAlpha (0.13f));
            g.fillPath (p);
        }

        // 0 dB line and the EQ curve (+/-18 dB over the height)
        const float mid = area.getCentreY();
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine (juce::roundToInt (mid), area.getX(), area.getRight());
        juce::Path c;
        for (int i = 0; i < (int) curve.size(); ++i)
        {
            const float y = mid - juce::jlimit (-24.0f, 24.0f, curve[(size_t) i]) / 18.0f * area.getHeight() * 0.5f;
            if (i == 0) c.startNewSubPath (xFor (curveHz (i)), y); else c.lineTo (xFor (curveHz (i)), y);
        }
        g.setColour (t.curve.withAlpha (0.9f));
        g.strokePath (c, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
        g.restoreState();
    }
};

//==============================================================================
InstancePanel::InstancePanel (StEqProcessor& o, InstanceHub& h, EqDisplay& d)
    : st::Popover ("Instances"), own (o), hub (h), ownDisplay (d)
{
    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (6);
    addAndMakeVisible (viewport);
    hub.onListChanged = [this] { rebuildRows(); };
    rebuildRows();
    startTimerHz (24);
}

InstancePanel::~InstancePanel()
{
    hub.onListChanged = nullptr;
    stopTimer();
}

void InstancePanel::rebuildRows()
{
    // keep rows of instances that are still there (their editors and expansion stay), add new ones
    std::vector<std::unique_ptr<Row>> next;
    auto take = [this] (st::ProcessorBase* p) -> std::unique_ptr<Row>
    {
        for (auto& r : rows)
            if (r != nullptr && r->getProcessor() == p) return std::move (r);
        return {};
    };

    auto ownRow = take (&own);
    if (ownRow == nullptr) ownRow = std::make_unique<Row> (*this, own, nullptr, &ownDisplay.getSpectrum (1));
    next.push_back (std::move (ownRow));
    for (auto& e : hub.getOthers())
    {
        auto row = take (e->proc);
        if (row == nullptr) row = std::make_unique<Row> (*this, *e->proc, e.get(), nullptr);
        next.push_back (std::move (row));
    }
    rows = std::move (next);   // rows of removed instances (and their embedded editors) are destroyed here

    bool expandedAlive = false;
    for (auto& r : rows)
    {
        holder.addAndMakeVisible (*r);
        expandedAlive |= r->getProcessor() == expanded;
    }
    if (! expandedAlive) expanded = nullptr;

    setTitle ("Instances", juce::String (rows.size()) + (rows.size() == 1 ? " instance" : " instances") + " in this session");
    resized();
    if (onHeightChanged) onHeightChanged();
}

void InstancePanel::toggleExpanded (st::ProcessorBase* p)
{
    expanded = expanded == p ? nullptr : p;
    for (auto& r : rows) r->setExpanded (r->getProcessor() == expanded);
    resized();
    if (onHeightChanged) onHeightChanged();
}

bool InstancePanel::hasInlineEditor() const
{
    return std::any_of (rows.begin(), rows.end(), [] (const auto& r) { return r->isExpanded(); });
}

int InstancePanel::rowsHeight() const
{
    int h = 0;
    for (auto& r : rows) h += r->getPreferredHeight() + 4;
    return h;
}

int InstancePanel::getPanelHeight() const
{
    return juce::jmin (430, headerHeight + 28 + rowsHeight());
}

void InstancePanel::layoutContent (juce::Rectangle<int> content)
{
    viewport.setBounds (content);
    const int w = content.getWidth() - (rowsHeight() > content.getHeight() ? 8 : 0);
    holder.setSize (w, rowsHeight());
    int y = 0;
    for (auto& r : rows)
    {
        r->setBounds (0, y, w, r->getPreferredHeight());
        y += r->getPreferredHeight() + 4;
    }
}

void InstancePanel::paintContent (juce::Graphics& g)
{
    if (rows.size() > 1) return;
    const auto& t = st::Theme::get();
    g.setFont (st::fonts::regular (12.0f));
    g.setColour (t.textDim);
    g.drawText ("Insert ST EQ on other tracks: they appear here, with their curves, collisions and inline editing.",
                getContentBounds().withTrimmedTop (Row::collapsedHeight + 10).toFloat(), juce::Justification::centredTop, true);
}

void InstancePanel::timerCallback()
{
    for (auto& r : rows) { r->refresh(); r->repaint(); }
}

} // namespace steq
