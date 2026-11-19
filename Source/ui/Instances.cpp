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
/** Small chrome button for the window header (paints its own glyph). */
class InstanceWindow::HeaderButton : public juce::Button
{
public:
    enum class Glyph { dots, sidebar, fullScreen, close, pin };
    HeaderButton (Glyph g, const juce::String& tip) : juce::Button (tip), glyph (g) { setTooltip (tip); setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& t = st::Theme::get();
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        const auto col = getToggleState() ? t.curve : (over || down ? t.text : t.textDim);
        g.setColour (col);
        switch (glyph)
        {
            case Glyph::dots:
                for (int i = -1; i <= 1; ++i) g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (r.getCentre().translated (0.0f, 5.0f * (float) i)));
                break;
            case Glyph::sidebar:
                g.drawRoundedRectangle (r.reduced (0.5f, 1.5f), 2.0f, 1.3f);
                g.fillRect (r.reduced (0.5f, 1.5f).removeFromRight (5.0f));
                break;
            case Glyph::fullScreen:
            {
                juce::Path p;
                const float a = 4.5f;
                const auto q = r.reduced (1.0f);
                p.startNewSubPath (q.getX(), q.getY() + a); p.lineTo (q.getX(), q.getY()); p.lineTo (q.getX() + a, q.getY());
                p.startNewSubPath (q.getRight() - a, q.getY()); p.lineTo (q.getRight(), q.getY()); p.lineTo (q.getRight(), q.getY() + a);
                p.startNewSubPath (q.getRight(), q.getBottom() - a); p.lineTo (q.getRight(), q.getBottom()); p.lineTo (q.getRight() - a, q.getBottom());
                p.startNewSubPath (q.getX() + a, q.getBottom()); p.lineTo (q.getX(), q.getBottom()); p.lineTo (q.getX(), q.getBottom() - a);
                g.strokePath (p, juce::PathStrokeType (1.4f));
                break;
            }
            case Glyph::close:
                st::icons::draw (g, st::icons::Icon::close, r.expanded (2.0f), col, 1.5f);
                break;
            case Glyph::pin:
            {
                // a push pin, tilted
                juce::Path p;
                p.addRoundedRectangle (-2.5f, -6.0f, 5.0f, 6.0f, 1.0f);
                p.startNewSubPath (-4.5f, 0.0f); p.lineTo (4.5f, 0.0f);
                p.startNewSubPath (0.0f, 0.0f); p.lineTo (0.0f, 6.0f);
                p.applyTransform (juce::AffineTransform::rotation (0.6f).translated (r.getCentre()));
                g.strokePath (p, juce::PathStrokeType (1.2f));
                break;
            }
        }
    }

private:
    Glyph glyph;
};

//==============================================================================
/** Header zoom: row size. */
class InstanceWindow::ZoomSlider : public juce::Component, public juce::SettableTooltipClient
{
public:
    float value = 0.35f;
    std::function<void()> onChange;
    ZoomSlider() { setTooltip ("Row size"); setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = st::Theme::get();
        auto r = getLocalBounds().toFloat().reduced (6.0f, 0.0f);
        const float y = r.getCentreY();
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.fillRoundedRectangle (r.withHeight (3.0f).withCentre ({ r.getCentreX(), y }), 1.5f);
        const float x = r.getX() + r.getWidth() * value;
        g.setColour (t.textDim);
        g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), y - 1.5f, x - r.getX(), 3.0f), 1.5f);
        g.setColour (t.text);
        g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ x, y }));
    }
    void mouseDown (const juce::MouseEvent& e) override { mouseDrag (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto r = getLocalBounds().toFloat().reduced (6.0f, 0.0f);
        value = juce::jlimit (0.0f, 1.0f, (e.position.x - r.getX()) / r.getWidth());
        repaint();
        if (onChange) onChange();
    }
};

//==============================================================================
/** One track: name chip (+ pin) on the left, its EQ display in a frame. */
class InstanceWindow::Row : public juce::Component
{
public:
    Row (InstanceWindow& w, StEqProcessor& p, InstanceHub::Entry* e)
        : owner (w), proc (p), entry (e), display (p, true),
          pin (HeaderButton::Glyph::pin, "Pin this track to the top of the list"),
          bypass (st::icons::Icon::power, "Bypass this track's EQ"),
          bypassAttachment (p.param (ids::bypass), bypass, &p.undoManager)
    {
        addAndMakeVisible (display);
        pin.setClickingTogglesState (false);
        pin.onClick = [this] { owner.togglePin (&proc); };
        addAndMakeVisible (pin);
        bypass.setMinimal (true);
        bypass.setClickingTogglesState (true);
        bypass.setOnColour (st::Theme::get().warning);
        addAndMakeVisible (bypass);
        height.snap (owner.baseRowHeight());
    }

    st::ProcessorBase* getProcessor() const noexcept { return &proc; }
    EqDisplay& getDisplay() noexcept { return display; }
    bool isOwn() const noexcept { return entry == nullptr; }
    st::Animated height;

    juce::Rectangle<int> frameBounds() const
    {
        const int h = getHeight();
        const int w = juce::jmin (getWidth() - nameWidth - 12, juce::roundToInt ((float) h * 3.9f));
        return { nameWidth, 0, juce::jmax (120, w), h };
    }

    void resized() override
    {
        const auto f = frameBounds();
        display.setBounds (f.withTrimmedLeft (20).reduced (1));
        bypass.setBounds (juce::Rectangle<int> (18, 18).withCentre ({ f.getX() + 10, f.getY() + 13 }));
        pin.setBounds (juce::Rectangle<int> (22, 22).withPosition (nameWidth - 34, getHeight() / 2 - 30));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = st::Theme::get();
        const auto f = frameBounds().toFloat();
        const auto col = instanceColour (proc);
        const bool pinnedRow = owner.pinned.contains (&proc);

        // name chip, right-aligned against the frame (own track bright)
        g.setFont (st::fonts::semibold (12.0f));
        const auto label = proc.getInstanceLabel();
        const float tw = st::fonts::stringWidth (st::fonts::semibold (12.0f), label) + 30.0f;
        auto chip = juce::Rectangle<float> (juce::jmin (tw, (float) nameWidth - 16.0f), 20.0f)
                        .withPosition ((float) nameWidth - 12.0f - juce::jmin (tw, (float) nameWidth - 16.0f), (float) getHeight() * 0.5f - 2.0f);
        g.setColour (isOwn() ? t.bg4 : t.bg3);
        g.fillRoundedRectangle (chip, 10.0f);
        g.setColour (isOwn() ? t.text : t.textDim);
        g.drawFittedText (label, chip.reduced (10.0f, 0.0f).withTrimmedRight (10.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (col);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ chip.getRight() - 11.0f, chip.getCentreY() }));
        if (pinnedRow)
        {
            g.setColour (t.curve.withAlpha (0.8f));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ (float) nameWidth - 23.0f, (float) getHeight() * 0.5f - 34.0f }));
        }

        // frame: dark card, vertical product label, collision glow
        g.setColour (t.bg1);
        g.fillRoundedRectangle (f, 6.0f);
        const float hit = entry != nullptr ? entry->collision : 0.0f;
        if (hit > 0.02f)
        {
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (t.danger.withAlpha (0.12f * hit / (float) i));
                g.drawRoundedRectangle (f.expanded ((float) i), 7.0f, (float) i * 1.6f);
            }
        }
        g.setColour (hit > 0.02f ? t.danger.withAlpha (0.35f + 0.5f * hit) : juce::Colours::white.withAlpha (isOwn() ? 0.14f : 0.07f));
        g.drawRoundedRectangle (f.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRect (f.withWidth (20.0f).withTrimmedTop (6.0f).withTrimmedBottom (6.0f).withX (f.getX() + 19.0f).withWidth (1.0f));
        {
            g.saveState();
            const auto c = juce::Point<float> (f.getX() + 10.0f, f.getBottom() - 30.0f);
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, c.x, c.y));
            g.setFont (st::fonts::semibold (8.5f));
            g.setColour (t.textFaint);
            g.drawText ("ST EQ", juce::Rectangle<float> (50.0f, 12.0f).withCentre (c), juce::Justification::centred, false);
            g.restoreState();
        }
    }

private:
    InstanceWindow& owner;
    StEqProcessor& proc;
    InstanceHub::Entry* entry;
    EqDisplay display;
    HeaderButton pin;
    st::IconButton bypass;
    juce::ButtonParameterAttachment bypassAttachment;
};

//==============================================================================
/** Track list on the right: every track with its colour; the tracks in view are boxed; click to scroll. */
class InstanceWindow::Sidebar : public juce::Component
{
public:
    explicit Sidebar (InstanceWindow& w) : owner (w) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    static constexpr float lineH = 18.0f, top = 10.0f;

    void paint (juce::Graphics& g) override
    {
        const auto& t = st::Theme::get();
        const auto view = owner.viewport.getViewArea();
        int first = -1, last = -1;
        for (int i = 0; i < (int) owner.rows.size(); ++i)
        {
            auto& r = *owner.rows[(size_t) i];
            if (! r.isVisible()) continue;
            if (r.getBounds().intersects (view)) { if (first < 0) first = i; last = i; }
        }
        if (first >= 0)
        {
            auto box = juce::Rectangle<float> (4.0f, top + lineH * (float) first - 2.0f, (float) getWidth() - 10.0f, lineH * (float) (last - first + 1) + 4.0f);
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRoundedRectangle (box, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);
        }
        for (int i = 0; i < (int) owner.rows.size(); ++i)
        {
            auto& r = *owner.rows[(size_t) i];
            const auto line = juce::Rectangle<float> (8.0f, top + lineH * (float) i, (float) getWidth() - 16.0f, lineH);
            g.setColour (instanceColour (*r.getProcessor()).withAlpha (r.isVisible() ? 1.0f : 0.35f));
            g.fillRect (line.withWidth (2.5f).reduced (0.0f, 3.0f));
            const bool inView = i >= first && i <= last;
            g.setFont (r.isOwn() ? st::fonts::semibold (11.5f) : st::fonts::regular (11.5f));
            g.setColour (! r.isVisible() ? t.textFaint.withAlpha (0.5f) : (inView || i == hover ? t.text : t.textDim));
            g.drawText (r.getProcessor()->getInstanceLabel(), line.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = indexAt (e.position.y); if (h != hover) { hover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = indexAt (e.position.y);
        if (i >= 0) owner.scrollTo (owner.rows[(size_t) i]->getProcessor());
    }

private:
    InstanceWindow& owner;
    int hover = -1;
    int indexAt (float y) const { const int i = (int) std::floor ((y - top) / lineH); return juce::isPositiveAndBelow (i, (int) owner.rows.size()) ? i : -1; }
};

//==============================================================================
InstanceWindow::InstanceWindow (StEqProcessor& o, InstanceHub& h) : own (o), hub (h)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (7);
    addAndMakeVisible (viewport);

    sidebar = std::make_unique<Sidebar> (*this);
    addAndMakeVisible (*sidebar);

    zoom = std::make_unique<ZoomSlider>();
    zoom->onChange = [this] { for (auto& r : rows) r->height.snap (baseRowHeight()); layoutRows(); };
    addAndMakeVisible (*zoom);

    const auto& t = st::Theme::get();
    search.setTextToShowWhenEmpty ("Search", t.textFaint);
    search.setJustification (juce::Justification::centred);
    search.setFont (st::fonts::regular (13.0f));
    search.setColour (juce::TextEditor::backgroundColourId, t.bg2);
    search.setColour (juce::TextEditor::textColourId, t.text);
    search.setColour (juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha (0.08f));
    search.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::white.withAlpha (0.2f));
    search.onTextChange = [this] { layoutRows(); };
    search.onEscapeKey = [this] { search.clear(); layoutRows(); grabKeyboardFocus(); };
    addAndMakeVisible (search);

    menuButton = std::make_unique<HeaderButton> (HeaderButton::Glyph::dots, "Options");
    sidebarButton = std::make_unique<HeaderButton> (HeaderButton::Glyph::sidebar, "Show / hide the track list");
    closeButton = std::make_unique<HeaderButton> (HeaderButton::Glyph::close, "Back to the EQ (Esc)");
    menuButton->onClick = [this] { showMenu(); };
    sidebarButton->onClick = [this] { showSidebar = ! showSidebar; resized(); };
    closeButton->onClick = [this] { if (onClose) onClose(); };
    for (auto* b : { menuButton.get(), sidebarButton.get(), closeButton.get() }) addAndMakeVisible (*b);

    hub.onListChanged = [this] { rebuildRows(); };
    rebuildRows();
    setSize (1040, 620);
    startTimerHz (60);
}

InstanceWindow::~InstanceWindow()
{
    hub.onListChanged = [] {};
    stopTimer();
    rows.clear();
}

float InstanceWindow::baseRowHeight() const
{
    return 70.0f + 170.0f * (zoom != nullptr ? zoom->value : 0.35f);
}

void InstanceWindow::rebuildRows()
{
    // keep rows (and their displays) of instances that are still there
    std::vector<std::unique_ptr<Row>> next;
    auto take = [this] (st::ProcessorBase* p) -> std::unique_ptr<Row>
    {
        for (auto& r : rows)
            if (r != nullptr && r->getProcessor() == p) return std::move (r);
        return {};
    };
    auto add = [&] (StEqProcessor& p, InstanceHub::Entry* e)
    {
        auto row = take (&p);
        if (row == nullptr) row = std::make_unique<Row> (*this, p, e);
        next.push_back (std::move (row));
    };
    add (own, nullptr);
    for (auto& e : hub.getOthers()) add (*e->proc, e.get());
    rows = std::move (next);   // rows of instances that are gone are destroyed here, before the instance is
    for (int i = pinned.size(); --i >= 0;)
        if (std::none_of (rows.begin(), rows.end(), [p = pinned[i]] (const auto& r) { return r->getProcessor() == p; })) pinned.remove (i);

    // order: pinned first, then session order (or by name)
    std::stable_sort (rows.begin(), rows.end(), [this] (const auto& a, const auto& b)
    {
        const bool pa = pinned.contains (a->getProcessor()), pb = pinned.contains (b->getProcessor());
        if (pa != pb) return pa;
        if (sortByName) return a->getProcessor()->getInstanceLabel().compareNatural (b->getProcessor()->getInstanceLabel()) < 0;
        return a->getProcessor()->getInstanceNumber() < b->getProcessor()->getInstanceNumber();
    });
    for (auto& r : rows) holder.addAndMakeVisible (*r);
    layoutRows();
    if (sidebar != nullptr) sidebar->repaint();
    repaint();
}

void InstanceWindow::layoutRows()
{
    const auto filter = search.getText().trim();
    const int w = viewport.getMaximumVisibleWidth();
    int y = 12;
    for (auto& r : rows)
    {
        const bool show = filter.isEmpty() || r->getProcessor()->getInstanceLabel().containsIgnoreCase (filter);
        r->setVisible (show);
        if (! show) continue;
        const int h = juce::roundToInt (r->height.current);
        r->setBounds (0, y, w, h);
        y += h + 14;
    }
    holder.setSize (w, y + 12);
    if (sidebar != nullptr) sidebar->repaint();
}

void InstanceWindow::scrollTo (st::ProcessorBase* p)
{
    for (auto& r : rows)
        if (r->getProcessor() == p && r->isVisible())
            viewport.setViewPosition (0, juce::jmax (0, r->getY() - 12));
    sidebar->repaint();
}

void InstanceWindow::togglePin (st::ProcessorBase* p)
{
    if (pinned.contains (p)) pinned.removeFirstMatchingValue (p);
    else pinned.add (p);
    rebuildRows();
}

void InstanceWindow::showMenu()
{
    juce::PopupMenu m;
    m.addItem (juce::PopupMenu::Item ("Sort by Track Name").setTicked (sortByName).setAction ([this] { sortByName = ! sortByName; rebuildRows(); }));
    m.addItem (juce::PopupMenu::Item ("Show Track List").setTicked (showSidebar).setAction ([this] { showSidebar = ! showSidebar; resized(); }));
    m.addSeparator();
    m.addItem (juce::PopupMenu::Item ("Compare Collisions with All Tracks").setAction ([this] { for (auto& e : hub.getOthers()) e->collide = true; }));
    m.addItem (juce::PopupMenu::Item ("Unpin All").setEnabled (! pinned.isEmpty()).setAction ([this] { pinned.clear(); rebuildRows(); }));
    st::showMenu (m, menuButton.get());
}

EqDisplay* InstanceWindow::getRowDisplay (st::ProcessorBase* p) const
{
    for (auto& r : rows)
        if (r->getProcessor() == p) return &r->getDisplay();
    return nullptr;
}

void InstanceWindow::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (headerHeight).reduced (14, 8);
    closeButton->setBounds (header.removeFromRight (26));
    header.removeFromRight (6);
    sidebarButton->setBounds (header.removeFromRight (26));
    sidebarButton->setToggleState (showSidebar, juce::dontSendNotification);

    const int cx = getWidth() / 2;
    search.setBounds (juce::Rectangle<int> (200, 26).withCentre ({ cx + 60, header.getCentreY() }));
    zoom->setBounds (juce::Rectangle<int> (90, 20).withCentre ({ search.getX() - 70, header.getCentreY() }));
    menuButton->setBounds (juce::Rectangle<int> (26, 26).withCentre ({ search.getRight() + 26, header.getCentreY() }));

    sidebar->setVisible (showSidebar);
    if (showSidebar) sidebar->setBounds (r.removeFromRight (sidebarWidth));
    viewport.setBounds (r.reduced (8, 0).withTrimmedBottom (8));
    layoutRows();
}

void InstanceWindow::paint (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    g.fillAll (t.bg0);
    auto header = getLocalBounds().removeFromTop (headerHeight).toFloat();
    g.setColour (t.bg1);
    g.fillRect (header);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRect (header.removeFromBottom (1.0f));

    // title, left
    g.setFont (st::fonts::bold (14.0f));
    g.setColour (t.text);
    g.drawText ("ST", juce::Rectangle<float> (18.0f, 0.0f, 30.0f, (float) headerHeight), juce::Justification::centredLeft, false);
    g.setFont (st::fonts::medium (14.0f));
    g.setColour (t.curve);
    g.drawText ("EQ", juce::Rectangle<float> (42.0f, 0.0f, 30.0f, (float) headerHeight), juce::Justification::centredLeft, false);
    g.setFont (st::fonts::regular (13.0f));
    g.setColour (t.textDim);
    g.drawText ("Instances  " + juce::String (rows.size()), juce::Rectangle<float> (74.0f, 0.0f, 160.0f, (float) headerHeight), juce::Justification::centredLeft, false);

    if (showSidebar)
    {
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRect ((float) getWidth() - (float) sidebarWidth, (float) headerHeight, 1.0f, (float) (getHeight() - headerHeight));
    }
}

bool InstanceWindow::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
    return false;
}

void InstanceWindow::timerCallback()
{
    // the row under the mouse grows a little (animated); everything else eases back
    st::ProcessorBase* hovered = forcedHover;
    if (hovered == nullptr && isShowing())
    {
        const auto pos = holder.getMouseXYRelative();
        if (viewport.getViewArea().contains (pos))
            for (auto& r : rows)
                if (r->isVisible() && r->getBounds().contains (pos)) hovered = r->getProcessor();
    }
    bool moving = false;
    const float base = baseRowHeight();
    for (auto& r : rows)
    {
        r->height.setTarget (r->getProcessor() == hovered ? base * 1.3f : base);
        moving |= r->height.tick (1.0 / 60.0, 14.0f);
    }
    if (moving) layoutRows();

    // collision glows / names follow the hub
    if (++ticks % 6 == 0)
    {
        for (auto& r : rows) if (r->isVisible()) r->repaint (0, 0, r->getWidth(), r->getHeight());
        sidebar->repaint();
    }
}

} // namespace steq
