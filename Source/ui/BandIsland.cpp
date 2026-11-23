#include "BandIsland.h"

namespace steq
{

juce::Path shapeGlyph (Shape s, juce::Rectangle<float> r)
{
    juce::Path p;
    const float x0 = r.getX(), x1 = r.getRight(), y0 = r.getY(), y1 = r.getBottom();
    const float mid = r.getCentreY(), cx = r.getCentreX(), w = r.getWidth();
    auto X = [&] (float t) { return x0 + w * t; };

    switch (s)
    {
        case Shape::bell:   // a simple arch
            p.startNewSubPath (X (0.12f), y1 - 1.0f);
            p.cubicTo (X (0.3f), y1 - 1.0f, X (0.36f), y0 + 1.0f, cx, y0 + 1.0f);
            p.cubicTo (X (0.64f), y0 + 1.0f, X (0.7f), y1 - 1.0f, X (0.88f), y1 - 1.0f);
            break;
        case Shape::lowShelf:
            p.startNewSubPath (x0, y0 + 1.0f);
            p.lineTo (X (0.3f), y0 + 1.0f);
            p.cubicTo (cx, y0 + 1.0f, cx, y1 - 1.0f, X (0.7f), y1 - 1.0f);
            p.lineTo (x1, y1 - 1.0f);
            break;
        case Shape::highShelf:
            p.startNewSubPath (x0, y1 - 1.0f);
            p.lineTo (X (0.3f), y1 - 1.0f);
            p.cubicTo (cx, y1 - 1.0f, cx, y0 + 1.0f, X (0.7f), y0 + 1.0f);
            p.lineTo (x1, y0 + 1.0f);
            break;
        case Shape::lowCut:
            p.startNewSubPath (X (0.12f), y1);
            p.cubicTo (X (0.3f), y0 + 2.0f, X (0.4f), y0 + 2.0f, X (0.55f), y0 + 2.0f);
            p.lineTo (x1, y0 + 2.0f);
            break;
        case Shape::highCut:
            p.startNewSubPath (x0, y0 + 2.0f);
            p.lineTo (X (0.45f), y0 + 2.0f);
            p.cubicTo (X (0.6f), y0 + 2.0f, X (0.7f), y0 + 2.0f, X (0.88f), y1);
            break;
        case Shape::notch:
            p.startNewSubPath (x0, y0 + 2.0f);
            p.lineTo (X (0.38f), y0 + 2.0f);
            p.cubicTo (X (0.47f), y0 + 2.0f, X (0.48f), y1, cx, y1);
            p.cubicTo (X (0.52f), y1, X (0.53f), y0 + 2.0f, X (0.62f), y0 + 2.0f);
            p.lineTo (x1, y0 + 2.0f);
            break;
        case Shape::bandPass:
            p.startNewSubPath (X (0.12f), y1);
            p.cubicTo (X (0.38f), y1 - 2.0f, X (0.4f), y0, cx, y0);
            p.cubicTo (X (0.6f), y0, X (0.62f), y1 - 2.0f, X (0.88f), y1);
            break;
        case Shape::tiltShelf:
            p.startNewSubPath (x0, y1 - 2.0f);
            p.lineTo (X (0.3f), y1 - 2.0f);
            p.cubicTo (cx, y1 - 2.0f, cx, y0 + 2.0f, X (0.7f), y0 + 2.0f);
            p.lineTo (x1, y0 + 2.0f);
            p.startNewSubPath (x0, mid); p.lineTo (X (0.12f), mid);
            break;
        case Shape::flatTilt:
            p.startNewSubPath (x0, y1 - 1.0f);
            p.lineTo (x1, y0 + 1.0f);
            break;
        case Shape::allPass:
            p.startNewSubPath (x0, mid);
            p.lineTo (X (0.3f), mid);
            p.addEllipse (cx - 3.5f, mid - 3.5f, 7.0f, 7.0f);
            p.startNewSubPath (X (0.7f), mid);
            p.lineTo (x1, mid);
            break;
    }
    return p;
}

//==============================================================================
namespace
{
    std::unique_ptr<juce::Drawable> iconFromPath (const juce::Path& p, float stroke, bool filled = false)
    {
        // fixed 24x24 frame so every glyph keeps its proportions inside the menu
        auto comp = std::make_unique<juce::DrawableComposite>();
        auto frame = std::make_unique<juce::DrawableRectangle>();
        frame->setRectangle (juce::Parallelogram<float> (juce::Rectangle<float> (0.0f, 0.0f, 24.0f, 24.0f)));
        frame->setFill (juce::Colours::transparentBlack);
        frame->setStrokeThickness (0.0f);
        auto glyph = std::make_unique<juce::DrawablePath>();
        glyph->setPath (p);
        glyph->setFill (filled ? juce::FillType (juce::Colours::black) : juce::FillType (juce::Colours::transparentBlack));
        glyph->setStrokeFill (juce::Colours::black);
        glyph->setStrokeType (juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        comp->addChild (std::move (frame));
        comp->addChild (std::move (glyph));
        comp->resetBoundingBoxToContentArea();
        return comp;
    }
}

std::unique_ptr<juce::Drawable> shapeMenuIcon (Shape s)
{
    return iconFromPath (shapeGlyph (s, juce::Rectangle<float> (2.0f, 6.0f, 20.0f, 12.0f)), 1.8f);
}

std::unique_ptr<juce::Drawable> placementMenuIcon (Placement p)
{
    if (p == Placement::stereo)
        return iconFromPath (st::icons::path (st::icons::Icon::stereoLink), 1.8f);
    static const char* letters[] = { "", "L", "R", "M", "S" };
    juce::Path path;
    path.addEllipse (3.0f, 3.0f, 18.0f, 18.0f);
    juce::GlyphArrangement ga;
    ga.addFittedText (st::fonts::bold (12.0f), letters[(int) p], 6.0f, 5.0f, 12.0f, 14.0f, juce::Justification::centred, 1);
    juce::Path text;
    ga.createPath (text);
    auto comp = iconFromPath (path, 1.6f);
    auto t = std::make_unique<juce::DrawablePath>();
    t->setPath (text);
    t->setFill (juce::Colours::black);
    dynamic_cast<juce::DrawableComposite*> (comp.get())->addChild (std::move (t));
    return comp;
}

juce::PopupMenu makeShapeMenu (int current)
{
    juce::PopupMenu m;
    for (int i = 0; i < shapeNames.size(); ++i)
        m.addItem (juce::PopupMenu::Item (shapeNames[i]).setID (i + 1).setTicked (i == current).setImage (shapeMenuIcon ((Shape) i)));
    return m;
}

//==============================================================================
namespace
{
    /** Dark tab on the island edge (shape / placement). */
    void paintTab (juce::Graphics& g, juce::Rectangle<float> r, bool hover, juce::Colour tint = {})
    {
        const auto& t = st::Theme::get();
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.0f), 5.0f);
        g.setGradientFill (juce::ColourGradient (t.bg4.withAlpha (hover ? 0.95f : 0.8f), 0.0f, r.getY(), t.bg3.withAlpha (0.9f), 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (hover ? 0.16f : 0.1f));
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
        if (! tint.isTransparent())
        {
            g.setColour (tint.withAlpha (hover ? 0.4f : 0.26f));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
        }
    }

    /** Small dark square button frame (power / delete). */
    void paintSmallBox (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto& t = st::Theme::get();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.0f), 4.0f);
        g.setColour (t.bg4.brighter (0.08f).withAlpha (0.95f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.11f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    }

    //==========================================================================
    /** Threshold meter + slider of the dynamics section: trigger level (green -> yellow), threshold handle.
        The top of the travel is "auto". Drag sets a manual threshold, double-click returns to auto. */
    struct ThresholdMeter : juce::Component, juce::SettableTooltipClient
    {
        StEqProcessor& proc;
        const int band;
        float level = -100.0f, thresh = -30.0f;
        bool dragging = false;
        static constexpr float lo = -80.0f, hi = 0.0f;

        ThresholdMeter (StEqProcessor& p, int b) : proc (p), band (b)
        {
            setTooltip ("Threshold: drag to set, double-click (or A) for automatic");
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        }

        juce::Rectangle<float> bar() const { return getLocalBounds().toFloat().withSizeKeepingCentre (5.0f, (float) getHeight() - 2.0f); }
        float yFor (float db) const { const auto b = bar(); return juce::jmap (juce::jlimit (lo, hi, db), lo, hi, b.getBottom(), b.getY() + 6.0f); }
        float dbFor (float y) const { const auto b = bar(); return juce::jmap (y, b.getBottom(), b.getY() + 6.0f, lo, hi); }

        void update (float l, float th) { if (std::abs (l - level) > 0.2f || std::abs (th - thresh) > 0.1f) { level = l; thresh = th; repaint(); } }

        void paint (juce::Graphics& g) override
        {
            const auto& t = st::Theme::get();
            const auto b = bar();
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillRoundedRectangle (b, 2.0f);
            if (level > lo)
            {
                const auto green = t.seriesColour (0).interpolatedWith (t.warning, 0.3f);
                g.setGradientFill (juce::ColourGradient (green.darker (0.2f), 0.0f, b.getBottom(), t.warning, 0.0f, b.getY(), false));
                g.fillRoundedRectangle (b.withTop (yFor (level)), 2.0f);
            }
            const bool autoOn = proc.getBand (band).dynAuto;
            const float y = yFor (thresh);
            g.setColour (autoOn ? t.textDim : t.text);
            g.fillRoundedRectangle (juce::Rectangle<float> ((float) getWidth() - 2.0f, 3.0f).withCentre ({ b.getCentreX(), y }), 1.5f);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragging = true;
            proc.undoManager.beginNewTransaction ("Threshold");
            proc.bandParam (band, "dyn_thresh").beginChangeGesture();
            mouseDrag (e);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging) return;
            auto& p = proc.bandParam (band, "dyn_thresh");
            if (proc.getBand (band).dynAuto) proc.setBandValue (band, "dyn_auto", 0.0f);
            p.setValueNotifyingHost (p.convertTo0to1 (juce::jlimit (lo, hi, dbFor (e.position.y))));
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging) proc.bandParam (band, "dyn_thresh").endChangeGesture();
            dragging = false;
        }
        void mouseDoubleClick (const juce::MouseEvent&) override { proc.setBandValue (band, "dyn_auto", 1.0f); }
    };

    /** "A" box above the threshold meter: automatic threshold on / off. */
    struct AutoBox : juce::Component, juce::SettableTooltipClient
    {
        StEqProcessor& proc;
        const int band;
        AutoBox (StEqProcessor& p, int b) : proc (p), band (b) { setTooltip ("Automatic threshold"); setMouseCursor (juce::MouseCursor::PointingHandCursor); }
        void paint (juce::Graphics& g) override
        {
            const auto& t = st::Theme::get();
            const bool on = proc.getBand (band).dynAuto;
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (on ? t.bg4.brighter (0.15f) : t.bg3);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (on ? 0.25f : 0.1f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            g.setFont (st::fonts::semibold (9.5f));
            g.setColour (on ? t.text : t.textFaint);
            g.drawText ("A", r, juce::Justification::centred, false);
        }
        void mouseDown (const juce::MouseEvent&) override
        {
            proc.undoManager.beginNewTransaction ("Auto threshold");
            proc.setBandValue (band, "dyn_auto", proc.getBand (band).dynAuto ? 0.0f : 1.0f);
            repaint();
        }
    };

    /** "⟷ Band" / "⟷ Free" trigger box. */
    struct TriggerButton : juce::Component, juce::SettableTooltipClient
    {
        StEqProcessor& proc;
        const int band;
        TriggerButton (StEqProcessor& p, int b) : proc (p), band (b)
        {
            setTooltip ("Trigger on the band's own range, or on a free range (drag its handles in the display)");
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }
        void paint (juce::Graphics& g) override
        {
            const auto& t = st::Theme::get();
            const bool free = proc.getBand (band).trigger == Trigger::free;
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            paintTab (g, r, isMouseOver());
            auto icon = r.removeFromLeft (24.0f).reduced (6.0f, 0.0f);
            const float y = icon.getCentreY();
            juce::Path p;
            p.startNewSubPath (icon.getX(), y); p.lineTo (icon.getRight(), y);
            p.startNewSubPath (icon.getX() + 3.0f, y - 3.0f); p.lineTo (icon.getX(), y); p.lineTo (icon.getX() + 3.0f, y + 3.0f);
            p.startNewSubPath (icon.getRight() - 3.0f, y - 3.0f); p.lineTo (icon.getRight(), y); p.lineTo (icon.getRight() - 3.0f, y + 3.0f);
            g.setColour (t.text.withAlpha (0.8f));
            g.strokePath (p, juce::PathStrokeType (1.2f));
            g.setFont (st::fonts::semibold (11.0f));
            g.setColour (t.text);
            g.drawText (free ? "Free" : "Band", r, juce::Justification::centredLeft, false);
        }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }
        void mouseDown (const juce::MouseEvent&) override
        {
            proc.undoManager.beginNewTransaction ("Trigger");
            proc.setBandValue (band, "dyn_trig", proc.getBand (band).trigger == Trigger::free ? 0.0f : 1.0f);
            repaint();
        }
    };
}

//==============================================================================
IslandButton::IslandButton (st::icons::Icon i, const juce::String& tip) : juce::Button (tip), icon (i)
{
    setTooltip (tip);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void IslandButton::timerCallback()
{
    hover.setTarget (isEnabled() ? (isDown() ? 1.0f : (isMouseOver() ? 0.85f : 0.0f)) : 0.0f);
    lit.setTarget (getToggleState() ? 1.0f : 0.0f);
    const bool a = hover.tick (1.0 / 60.0, 16.0f);
    const bool b = lit.tick (1.0 / 60.0, 12.0f);
    const bool moving = a || b;
    repaint();
    if (! moving) stopTimer();
}

void IslandButton::paintButton (juce::Graphics& g, bool, bool)
{
    const auto& t = st::Theme::get();
    auto r = getLocalBounds().toFloat();
    const auto area = r.reduced (juce::jmax (2.5f, r.getHeight() * 0.17f));
    const float h = hover.current, on = lit.current;

    // rests dim, eases to white on hover; "on" lights in its colour with a soft glow
    auto col = t.textDim.withAlpha (isEnabled() ? 0.75f : 0.35f).interpolatedWith (t.text, h);
    if (on > 0.001f && ! onColour.isTransparent())
    {
        col = col.interpolatedWith (onColour.brighter (0.15f * h), on);
        g.setColour (onColour.withAlpha (0.07f * on));
        g.fillEllipse (area.expanded (1.0f));
    }
    st::icons::draw (g, icon, area, col, 1.5f);
}

//==============================================================================
ShapeButton::ShapeButton (juce::RangedAudioParameter& p, juce::UndoManager& um, juce::Colour c)
    : undo (um), attachment (p, [this] (float v) { index = juce::roundToInt (v); repaint(); }, &um), tint (c)
{
    attachment.sendInitialUpdate();
    setTooltip ("Filter shape (wheel to step)");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void ShapeButton::setIndex (int i)
{
    undo.beginNewTransaction ("Shape");
    attachment.setValueAsCompleteGesture ((float) juce::jlimit (0, shapeNames.size() - 1, i));
}

void ShapeButton::paint (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    paintTab (g, r, isMouseOver(), tint);
    auto glyph = r.removeFromLeft (26.0f).reduced (6.5f, 7.0f).translated (3.0f, 0.0f);
    g.setColour (t.text.withAlpha (0.75f));
    g.strokePath (shapeGlyph ((Shape) index, glyph), juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setFont (st::fonts::medium (14.5f * BandPanel::kScale));
    g.setColour (t.textDim);
    g.drawFittedText (shapeNames[index], r.withTrimmedLeft (2.0f).withTrimmedRight (4.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
}

void ShapeButton::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    st::showMenu (makeShapeMenu (index), this, [safe = juce::Component::SafePointer<ShapeButton> (this)] (int r) { if (safe != nullptr && r > 0) safe->setIndex (r - 1); });
}

void ShapeButton::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX;
    if (std::abs (d) < 0.01f) return;
    setIndex ((index + (d > 0 ? -1 : 1) + shapeNames.size()) % shapeNames.size());
}

//==============================================================================
PlacementButton::PlacementButton (juce::RangedAudioParameter& p, juce::UndoManager& um, juce::Colour c)
    : undo (um), attachment (p, [this] (float v) { index = juce::roundToInt (v); repaint(); }, &um), colour (c)
{
    attachment.sendInitialUpdate();
    setTooltip ("Stereo placement: both channels, left, right, mid or side only");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void PlacementButton::paint (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    paintTab (g, r, isMouseOver(), colour);
    auto icon = r.removeFromLeft (28.0f).reduced (6.0f, 5.0f).translated (2.0f, 0.0f);
    if (index == 0)
        st::icons::draw (g, st::icons::Icon::stereoLink, icon, t.text.withAlpha (0.75f), 1.3f);
    else
    {
        static const char* letters[] = { "", "L", "R", "M", "S" };
        const auto c = icon.withSizeKeepingCentre (14.0f, 14.0f);
        g.setColour (t.text.withAlpha (0.75f));
        g.drawEllipse (c, 1.2f);
        g.setFont (st::fonts::bold (8.5f));
        g.drawText (letters[index], c, juce::Justification::centred, false);
    }
    g.setColour (colour);
    g.fillEllipse (juce::Rectangle<float> (4.5f, 4.5f).withCentre ({ r.getX() + 3.0f, r.getCentreY() }));
    g.setFont (st::fonts::medium (14.5f * BandPanel::kScale));
    g.setColour (t.textDim);
    g.drawFittedText (placementNames[index], r.withTrimmedLeft (10.0f).withTrimmedRight (4.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
}

void PlacementButton::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    juce::PopupMenu m;
    for (int i = 0; i < placementNames.size(); ++i)
        m.addItem (juce::PopupMenu::Item (placementNames[i]).setID (i + 1).setTicked (i == index).setImage (placementMenuIcon ((Placement) i)));
    st::showMenu (m, this, [safe = juce::Component::SafePointer<PlacementButton> (this)] (int r)
    {
        if (safe == nullptr || r <= 0) return;
        safe->undo.beginNewTransaction ("Stereo placement");
        safe->attachment.setValueAsCompleteGesture ((float) (r - 1));
    });
}

//==============================================================================
BandPanel::BandPanel (StEqProcessor& p, int b, int selected, bool expand) : proc (p), band (b), numSelected (selected)
{
    auto* um = &proc.undoManager;
    auto P = [this] (const char* s) -> juce::RangedAudioParameter& { return proc.bandParam (band, s); };
    const auto col = bandColour (band);
    const auto& t = st::Theme::get();

    shape = std::make_unique<ShapeButton> (P ("shape"), proc.undoManager, col);
    placement = std::make_unique<PlacementButton> (P ("place"), proc.undoManager, col);
    slope = std::make_unique<st::Dropdown> (P ("slope"), um);
    slope->setFlatStyle (true);
    slope->setTooltip ("Filter slope (mouse wheel over a cut filter's dot steps it)");

    auto knob = [&] (std::unique_ptr<st::Knob>& k, const char* id, const char* tip)
    {
        k = std::make_unique<st::Knob> (P (id), um, "");
        k->setSize (st::Knob::Size::mini);
        k->setLook (st::Knob::Look::arc);
        k->setColour (col);
        k->setTooltip (tip);
        addAndMakeVisible (*k);
    };
    knob (freq, "freq", "Frequency (double-click a dot to type a value or a note)");
    knob (gain, "gain", "Gain. Drag the outer ring for dynamic EQ (range).");
    knob (q, "q", "Bandwidth / resonance (mouse wheel over the dot)");
    knob (attack, "dyn_attack", "Attack");
    knob (release, "dyn_release", "Release");
    knob (density, "spec_density", "Spectral density: low = neighbouring frequencies react together, high = each frequency on its own");
    knob (depth, "depth", "Depth (3D mode): moves the band back. Boosts get wider with small room reflections; cuts leave the centre and move to the sides");
    gain->setRecessed (true);
    gain->setBipolar (true);
    q->setBipolar (true);   // Q = 1 at twelve o'clock, arc grows either way
    gain->setRimParameter (&P ("dyn_range"), "Range", false);
    gain->setRimStyle (st::Knob::RimStyle::range);
    gain->setRimActivitySource ([this] { return proc.bandMeter (band).dynDb.load (std::memory_order_relaxed); });

    threshold = std::make_unique<ThresholdMeter> (proc, band);
    autoBox = std::make_unique<AutoBox> (proc, band);
    triggerButton = std::make_unique<TriggerButton> (proc, band);

    for (juce::Component* c : { (juce::Component*) shape.get(), (juce::Component*) placement.get(), (juce::Component*) slope.get(),
                                threshold.get(), autoBox.get(), triggerButton.get() })
        addAndMakeVisible (c);

    for (auto* ib : { &bypassButton, &closeButton, &prevButton, &nextButton, &gainQButton, &splitButton, &clearDynButton,
                      &dynBypassButton, &spectralButton, &tiltButton, &expandButton, &collapseButton, &sidechainButton, &auditionButton })
    {
        ib->setMinimal (true);
        addAndMakeVisible (*ib);
    }

    bypassButton.setOnColour (t.accent2);
    bypassButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Bypass band"); proc.setBandValue (band, "bypass", proc.getBand (band).bypass ? 0.0f : 1.0f); };
    closeButton.onClick = [this] { if (onRemove) onRemove(); };
    prevButton.onClick = [this] { if (onPrevious) onPrevious(); };
    nextButton.onClick = [this] { if (onNext) onNext(); };
    gainQButton.setOnColour (col);
    gainQButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Gain-Q"); proc.setBandValue (band, "gq", proc.getBand (band).gainQ ? 0.0f : 1.0f); };
    splitButton.onClick = [this] { if (onSplit) onSplit(); };
    clearDynButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Clear dynamics"); proc.setBandValue (band, "dyn_range", 0.0f); setExpanded (false); };
    dynBypassButton.setOnColour (t.accent2);
    dynBypassButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Dynamics bypass"); proc.setBandValue (band, "dyn_bypass", proc.getBand (band).dynBypass ? 0.0f : 1.0f); };
    spectralButton.setOnColour (col.brighter (0.2f));
    spectralButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Spectral dynamics"); proc.setBandValue (band, "spectral", proc.getBand (band).spectral ? 0.0f : 1.0f); };
    tiltButton.setOnColour (col.brighter (0.2f));
    tiltButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Spectral tilt"); proc.setBandValue (band, "spec_tilt", proc.getBand (band).specTilt ? 0.0f : 1.0f); };
    expandButton.onClick = [this] { setExpanded (true); };
    collapseButton.onClick = [this] { setExpanded (false); };
    sidechainButton.setOnColour (t.seriesColour (1));
    sidechainButton.onClick = [this] { proc.undoManager.beginNewTransaction ("Side chain"); proc.setBandValue (band, "dyn_sc", proc.getBand (band).sidechain ? 0.0f : 1.0f); };
    auditionButton.setOnColour (t.warning);
    auditionButton.onStateChange = [this]
    {
        if (auditionButton.isDown()) { proc.soloTrigger = true; proc.soloBand = band; }
        else if (proc.soloTrigger.load() && proc.soloBand.load() == band) { proc.soloBand = -1; proc.soloTrigger = false; }
    };

    expanded = expand && isDynamic();
    dynAmt.snap (isDynamic() ? 1.0f : 0.0f);
    expAmt.snap (expanded ? 1.0f : 0.0f);
    specAmt.snap (proc.getBand (band).spectral ? 1.0f : 0.0f);
    last3d = proc.refs.is3D();
    depthAmt.snap (last3d ? 1.0f : 0.0f);
    setSize (getPreferredWidth(), height);
    updateState();
    startTimerHz (60);
}

BandPanel::~BandPanel()
{
    stopTimer();
    if (proc.soloTrigger.load() && proc.soloBand.load() == band) { proc.soloBand = -1; proc.soloTrigger = false; }
}

bool BandPanel::isDynamic() const
{
    const auto p = proc.getBand (band);
    return st::dsp::eq::hasGain (p.shape) && std::abs (p.dynRange) > 0.05f;
}

void BandPanel::setExpanded (bool e)
{
    e = e && isDynamic();
    if (e == expanded) return;
    expanded = e;
    expAmt.setTarget (e ? 1.0f : 0.0f);
    animating = true;
    updateState();
    if (onSizeChanged) onSizeChanged();
}

void BandPanel::animateStep (double dt)
{
    const bool a = dynAmt.tick (dt, 14.0f);
    const bool b = expAmt.tick (dt, 14.0f);
    const bool c = specAmt.tick (dt, 14.0f);
    const bool d = depthAmt.tick (dt, 14.0f);
    const bool moving = a || b || c || d;
    backgroundCache = {};
    if (getWidth() != getPreferredWidth()) setSize (getPreferredWidth(), height);
    else resized();
    repaint();
    if (! moving) animating = false;
}

juce::Rectangle<int> BandPanel::box (float x, float y, float w, float h) const noexcept
{
    return juce::Rectangle<float> (w * kScale, h * kScale).withPosition (toLocal (x, y)).toNearestInt();
}

void BandPanel::timerCallback()
{
    if (animating) animateStep (1.0 / 60.0);
    const auto p = proc.getBand (band);
    const bool dyn = isDynamic();
    if (proc.refs.is3D() != last3d)
    {
        last3d = ! last3d;
        depthAmt.setTarget (last3d ? 1.0f : 0.0f);
        animating = true;
        if (onSizeChanged) onSizeChanged();
    }
    if (p.shape != lastShape || dyn != lastDynamic || p.bypass != lastBypass || p.dynBypass != lastDynBypass || p.gainQ != lastGq
        || p.sidechain != lastSc || p.spectral != lastSpectral || p.specTilt != lastTilt)
    {
        if (! dyn && expanded) setExpanded (false);
        updateState();
    }

    // knob captions swap to values while hovered / dragged (they're drawn by the island)
    int mask = 0, bit = 1;
    for (auto* k : { freq.get(), gain.get(), q.get(), attack.get(), release.get(), density.get(), depth.get() })
    {
        if (k->isMouseOverOrDragging()) mask |= bit;
        bit <<= 1;
    }
    if (mask != 0 || mask != lastHoverMask) repaint();
    lastHoverMask = mask;

    if (expanded)
    {
        auto& m = proc.bandMeter (band);
        static_cast<ThresholdMeter*> (threshold.get())->update (m.levelDb.load(), m.thresholdDb.load());
        autoBox->repaint();
    }
}

void BandPanel::updateState()
{
    const auto p = proc.getBand (band);
    lastShape = p.shape;
    lastDynamic = isDynamic();
    lastBypass = p.bypass;
    lastDynBypass = p.dynBypass;
    lastGq = p.gainQ;
    lastSc = p.sidechain;
    lastSpectral = p.spectral;
    lastTilt = p.specTilt;

    const bool hasGain = st::dsp::eq::hasGain (p.shape);
    slope->setVisible (st::dsp::eq::hasSlope (p.shape));
    gain->setEnabled (hasGain);
    gain->setAlpha (hasGain ? 1.0f : 0.35f);
    q->setEnabled (st::dsp::eq::hasQ (p.shape));
    q->setAlpha (st::dsp::eq::hasQ (p.shape) ? 1.0f : 0.35f);
    gainQButton.setVisible (p.shape == Shape::bell);
    gainQButton.setToggleState (p.gainQ, juce::dontSendNotification);
    bypassButton.setToggleState (p.bypass, juce::dontSendNotification);

    if (! juce::exactlyEqual (dynAmt.target, lastDynamic ? 1.0f : 0.0f)) { dynAmt.setTarget (lastDynamic ? 1.0f : 0.0f); animating = true; }
    dynBypassButton.setToggleState (p.dynBypass, juce::dontSendNotification);
    spectralButton.setToggleState (p.spectral, juce::dontSendNotification);
    tiltButton.setToggleState (p.specTilt, juce::dontSendNotification);
    if (! juce::exactlyEqual (specAmt.target, p.spectral ? 1.0f : 0.0f)) { specAmt.setTarget (p.spectral ? 1.0f : 0.0f); animating = true; }
    sidechainButton.setToggleState (p.sidechain, juce::dontSendNotification);
    splitButton.setEnabled (p.place == Placement::stereo);

    backgroundCache = {};
    resized();
    repaint();
}

void BandPanel::buildOutline()
{
    // Reference units (see header). Measured from the original: plain bands have a smooth bell-shaped rise to a
    // peak over the gain knob; dynamic bands a gentle arch into a tab with a chevron tip; expanded, the tab runs
    // over the dynamics section. All three share one point structure so the outline can morph between them.
    struct Top { juce::Point<float> c1a, c1b, p1, c2a, c2b, p2, p3, p4, p5, c5a, c5b; };
    const float W = (float) collapsedWidth + shift();
    const Top plain     { { 100, -2 }, { 150, -20 }, { 190, -25 }, { 205, -27 }, { 215, -26 }, { 233.5f, -26 }, { 277, -25 }, { 290, -22 },
                          { 300, -19 }, { 345, -10 }, { 400, -2 } };
    const Top dynamic   { { 45, -2 },  { 125, -7 },  { 176, -16.5f }, { 182, -19 }, { 186, tabTop }, { 196, tabTop }, { 276, tabTop },
                          { 286.5f, -11 }, { 293, -4.5f }, { 335, -3 }, { 420, 0 } };
    const Top expandedT { { 45, -2 },  { 125, -7 },  { 176, -16.5f }, { 182, -19 }, { 186, tabTop }, { 196, tabTop }, { 410, tabTop },
                          { 424, -14 }, { 440, -5 }, { 500, -3 }, { 568, 0 } };

    auto mix = [] (const Top& a, const Top& b, float t)
    {
        auto m = [t] (juce::Point<float> x, juce::Point<float> y) { return x + (y - x) * t; };
        return Top { m (a.c1a, b.c1a), m (a.c1b, b.c1b), m (a.p1, b.p1), m (a.c2a, b.c2a), m (a.c2b, b.c2b), m (a.p2, b.p2),
                     m (a.p3, b.p3), m (a.p4, b.p4), m (a.p5, b.p5), m (a.c5a, b.c5a), m (a.c5b, b.c5b) };
    };
    const Top top = mix (mix (plain, dynamic, dynAmt.current), expandedT, expAmt.current);

    const float H = bodyHeight, r = 10.0f;
    auto P = [this] (juce::Point<float> pt) { return toLocal (pt.x, pt.y); };
    auto Q = [this] (float x, float y) { return toLocal (x, y); };

    juce::Path p;
    p.startNewSubPath (Q (0.0f, r));
    p.quadraticTo (Q (0.0f, 0.0f), Q (r, 0.0f));
    p.cubicTo (P (top.c1a), P (top.c1b), P (top.p1));
    p.cubicTo (P (top.c2a), P (top.c2b), P (top.p2));
    p.lineTo (P (top.p3));
    p.lineTo (P (top.p4));
    p.lineTo (P (top.p5));
    p.cubicTo (P (top.c5a), P (top.c5b), Q (W - r, 0.0f));
    p.quadraticTo (Q (W, 0.0f), Q (W, r));
    p.lineTo (Q (W, H - r));
    p.quadraticTo (Q (W, H), Q (W - r, H));
    p.quadraticTo (Q (gainCentreX + shift() * 0.5f, H + 2.0f * bulge), Q (r, H));
    p.quadraticTo (Q (0.0f, H), Q (0.0f, H - r));
    p.closeSubPath();
    outline = p;
}

void BandPanel::resized()
{
    buildOutline();
    const float sh = shift();
    auto centred = [this] (juce::Component& c, float cx, float cy, float w, float h) { c.setBounds (box (cx - w * 0.5f, cy - h * 0.5f, w, h)); };

    // left column
    bypassButton.setBounds (box (6.0f, 3.0f, 21.0f, 21.0f));
    shape->setBounds (box (0.0f, 37.0f, 91.0f, 24.0f));
    slope->setBounds (box (2.0f, 68.0f, 88.0f, 18.0f));

    // knobs: dial bounds only, captions are drawn by the island
    auto knobBox = [this] (st::Knob& k, float cx, float cy, float discR, bool recessed)
    {
        // Knob::geometry: outerR = R - max (2, 0.07R) - 1.5; disc = outerR * (recessed ? 0.74 : 0.96)
        const float outer = discR * kScale / (recessed ? 0.74f : 0.96f);
        const float R = (outer + 1.5f) / 0.93f;
        const auto c = toLocal (cx, cy);
        k.setBounds (juce::Rectangle<float> (2.0f * R, 2.0f * R).withCentre (c).toNearestInt());
    };
    knobBox (*freq, 140.0f, 45.4f, 28.3f, false);
    knobBox (*gain, gainCentreX, 39.4f, 32.7f, true);
    knobBox (*q, 328.7f + dynShift(), 43.9f, 28.3f, false);
    knobBox (*depth, depthX(), 43.9f, 24.0f, false);
    // attack / release (+ density for spectral bands: the three close up)
    const float sp = specAmt.current;
    knobBox (*attack, attackX(), 59.0f, 15.0f - 2.0f * sp, false);
    knobBox (*release, releaseX(), 59.0f, 15.0f - 2.0f * sp, false);
    knobBox (*density, 387.0f, 59.0f, 13.0f, false);

    // tab icons
    centred (clearDynButton, 177.7f, -4.5f, 21.0f, 21.0f);
    centred (dynBypassButton, 205.0f, -10.4f, 21.0f, 21.0f);
    centred (spectralButton, 233.5f, -14.9f, 25.0f, 21.0f);
    centred (expandButton, 266.0f, -11.9f, 21.0f, 21.0f);
    centred (collapseButton, 276.0f, -13.0f, 19.0f, 19.0f);

    // dynamics section (expanded)
    centred (*autoBox, 304.0f, -12.0f, 18.0f, 12.0f);
    threshold->setBounds (box (297.0f, -3.0f, 14.0f, 98.0f));
    centred (sidechainButton, 327.0f, -11.0f, 18.0f, 18.0f);
    centred (auditionButton, 356.0f, -11.0f, 18.0f, 18.0f);
    centred (tiltButton, 385.0f, -11.0f, 18.0f, 18.0f);
    triggerButton->setBounds (box (321.0f, 8.0f, 70.0f, 20.0f));

    // right column
    centred (gainQButton, 293.0f + dynShift(), 20.0f, 24.0f, 22.0f);
    centred (prevButton, 390.0f + sh, 12.0f, 15.0f, 18.0f);
    centred (nextButton, 420.0f + sh, 12.0f, 15.0f, 18.0f);
    closeButton.setBounds (box (443.0f + sh, 3.0f, 21.0f, 21.0f));
    placement->setBounds (box (377.0f + sh, 37.0f, 93.0f, 24.0f));
    centred (splitButton, 423.0f + sh, 78.0f, 19.0f, 19.0f);

    // cross-fades: the tab icons follow the dynamics amount, the dynamics section the expand amount
    auto fade = [] (juce::Component& c, float a)
    {
        c.setAlpha (juce::jlimit (0.0f, 1.0f, a));
        c.setVisible (a > 0.02f);
        c.setInterceptsMouseClicks (a > 0.5f, a > 0.5f);
    };
    const float d = dynAmt.current, x = expAmt.current;
    for (auto* c : std::initializer_list<juce::Component*> { &clearDynButton, &dynBypassButton, &spectralButton })
        fade (*c, d);
    fade (expandButton, d * (1.0f - x));
    fade (collapseButton, x);
    for (auto* c : std::initializer_list<juce::Component*> { threshold.get(), autoBox.get(), triggerButton.get(), attack.get(), release.get(), &sidechainButton, &auditionButton })
        fade (*c, x * x);
    for (auto* c : std::initializer_list<juce::Component*> { density.get(), &tiltButton })
        fade (*c, x * x * sp);
    fade (*depth, depthAmt.current * depthAmt.current);
}

void BandPanel::paintBackground (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const auto col = bandColour (band);

    // soft shadow inside our bounds
    for (int i = 4; i >= 1; --i)
    {
        g.setColour (juce::Colours::black.withAlpha (0.09f));
        g.fillPath (outline, juce::AffineTransform::translation (0.0f, (float) i * 1.6f));
    }

    // dark translucent glass, a little lighter towards the top
    const auto top = toLocal (0.0f, tabTop).y, bottom = toLocal (0.0f, bodyHeight + bulge).y;
    g.setGradientFill (juce::ColourGradient (t.bg3.interpolatedWith (col, 0.045f).withAlpha (0.88f), 0.0f, top,
                                             t.bg2.interpolatedWith (col, 0.025f).withAlpha (0.93f), 0.0f, bottom, false));
    g.fillPath (outline);

    // hairline edge all round, and along the top a light tinted by the band's colour (fading at the ends)
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.strokePath (outline, juce::PathStrokeType (1.0f));
    {
        g.saveState();
        g.reduceClipRegion (juce::Rectangle<int> (0, 0, getWidth(), (int) toLocal (0.0f, 9.0f).y));
        const float x0 = toLocal (0.0f, 0.0f).x, x1 = toLocal ((float) collapsedWidth + shift(), 0.0f).x;
        const auto edgeCol = col.interpolatedWith (juce::Colours::white, 0.35f);
        juce::ColourGradient edge (edgeCol.withAlpha (0.0f), x0, 0.0f, edgeCol.withAlpha (0.0f), x1, 0.0f, false);
        edge.addColour (0.3, edgeCol.withAlpha (0.30f));
        edge.addColour (0.55, edgeCol.withAlpha (0.38f));
        edge.addColour (0.8, edgeCol.withAlpha (0.22f));
        g.setGradientFill (edge);
        g.strokePath (outline, juce::PathStrokeType (1.2f));
        g.restoreState();
    }

    // seams of the expanded dynamics section
    if (expAmt.current > 0.02f)
    {
        juce::Path seam;
        seam.startNewSubPath (toLocal (285.0f, tabTop));
        seam.lineTo (toLocal (291.0f, -21.0f));
        seam.lineTo (toLocal (287.0f, -11.0f));
        seam.startNewSubPath (toLocal (420.0f, -16.0f));
        seam.cubicTo (toLocal (432.0f, 16.0f), toLocal (432.0f, 66.0f), toLocal (426.0f, bodyHeight + 6.0f));
        g.setColour (juce::Colours::white.withAlpha (0.06f * expAmt.current));
        g.strokePath (seam, juce::PathStrokeType (1.0f));
    }

    // small boxes behind the power and delete icons
    paintSmallBox (g, bypassButton.getBounds().toFloat());
    paintSmallBox (g, closeButton.getBounds().toFloat());
}

void BandPanel::paint (juce::Graphics& g)
{
    const float ps = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (backgroundCache.isNull() || ! juce::exactlyEqual (ps, cacheScale) || backgroundCache.getWidth() != juce::roundToInt ((float) getWidth() * ps))
    {
        cacheScale = ps;
        backgroundCache = juce::Image (juce::Image::ARGB, juce::roundToInt ((float) getWidth() * ps), juce::roundToInt ((float) getHeight() * ps), true);
        juce::Graphics bg (backgroundCache);
        bg.addTransform (juce::AffineTransform::scale (ps));
        paintBackground (bg);
    }
    g.drawImage (backgroundCache, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
}

void BandPanel::paintLabel (juce::Graphics& g, st::Knob& k, const juce::String& label, juce::Point<float> c)
{
    const auto& t = st::Theme::get();
    const bool showValue = k.isMouseOverOrDragging() && k.isEnabled();
    g.setFont (st::fonts::semibold (fs (12.5f)));
    g.setColour (k.isEnabled() ? t.text.withAlpha (0.92f) : t.textFaint);
    g.drawText (showValue ? k.getParameter().getCurrentValueAsText() : label,
                juce::Rectangle<float> (90.0f, 14.0f).withCentre (c), juce::Justification::centred, false);
}

void BandPanel::paintOverChildren (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const auto p = proc.getBand (band);
    const float sh = shift();

    paintLabel (g, *freq, "FREQ", toLocal (140.0f, 84.0f));
    paintLabel (g, *gain, "GAIN", toLocal (gainCentreX, 91.5f));
    paintLabel (g, *q, "Q", toLocal (328.7f + dynShift(), 84.0f));
    if (depthAmt.current > 0.02f)
    {
        g.saveState();
        g.setOpacity (depthAmt.current);
        paintLabel (g, *depth, "DEPTH", toLocal (depthX(), 84.0f));
        g.restoreState();
    }

    // centre references: "0 dB" over Gain on the plateau (when there is no dynamics tab), "1" over Q
    g.setFont (st::fonts::medium (fs (10.5f)));
    g.setColour (t.textDim);
    if (dynAmt.current < 0.98f && st::dsp::eq::hasGain (p.shape))
    {
        g.setColour (t.textDim.withAlpha (1.0f - dynAmt.current));
        g.drawText ("0 dB", juce::Rectangle<float> (40.0f, 12.0f).withCentre (toLocal (gainCentreX, -12.0f)), juce::Justification::centred, false);
        g.fillRect (juce::Rectangle<float> (1.0f, 3.0f).withCentre (toLocal (gainCentreX, -5.5f)));
    }
    g.setColour (t.textDim);
    if (st::dsp::eq::hasQ (p.shape))
        g.drawText ("1", juce::Rectangle<float> (20.0f, 12.0f).withCentre (toLocal (328.7f + dynShift(), 6.0f)), juce::Justification::centred, false);

    // range grip marks on the gain well (top left), hinting that the ring can be dragged
    if (st::dsp::eq::hasGain (p.shape))
    {
        const auto c = toLocal (gainCentreX, 39.4f);
        const float a = -0.95f;
        const auto pos = c.getPointOnCircumference (40.0f * kScale, a);
        g.setColour (t.text.withAlpha (0.4f));
        for (int i = -1; i <= 1; i += 2)
        {
            const auto o = juce::Point<float> (std::cos (a), std::sin (a)) * (float) i * 1.8f;
            const auto d = juce::Point<float> (std::sin (a), -std::cos (a)) * 2.2f;
            g.drawLine (juce::Line<float> (pos + o - d, pos + o + d), 1.0f);
        }
    }

    // "(multiple)" when several bands are edited and there is no slope to show
    if (numSelected > 1 && ! slope->isVisible())
    {
        g.setFont (st::fonts::medium (fs (12.0f)));
        g.setColour (t.textDim);
        g.drawText ("(multiple)", slope->getBounds().toFloat(), juce::Justification::centred, false);
    }

    // band number between the nav arrows (dim, like the arrows)
    g.setFont (st::fonts::medium (fs (11.5f)));
    g.setColour (p.bypass ? t.textFaint : t.textDim);
    g.drawText (juce::String (band + 1), juce::Rectangle<float> (20.0f, 14.0f).withCentre (toLocal (405.0f + sh, 12.0f)), juce::Justification::centred, false);

    if (expAmt.current > 0.02f)
    {
        g.setFont (st::fonts::semibold (fs (10.5f)));
        g.setColour (t.textDim.withAlpha (expAmt.current * expAmt.current));
        auto caption = [&] (st::Knob& k, const char* text, float x)
        {
            g.drawText (k.isMouseOverOrDragging() ? k.getParameter().getCurrentValueAsText() : juce::String (text),
                        juce::Rectangle<float> (60.0f, 12.0f).withCentre (toLocal (x, 84.0f)), juce::Justification::centred, false);
        };
        caption (*attack, "A", attackX());
        caption (*release, "R", releaseX());
        if (specAmt.current > 0.02f)
        {
            g.setColour (t.textDim.withAlpha (expAmt.current * expAmt.current * specAmt.current));
            caption (*density, "DENS", 387.0f);
        }
    }
}

} // namespace steq
