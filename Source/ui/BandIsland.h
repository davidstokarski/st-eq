#pragma once
#include "../PluginProcessor.h"

namespace steq
{

/** Small glyph of a filter shape in `area` (shape button, menus). */
juce::Path shapeGlyph (Shape, juce::Rectangle<float> area);
/** Menu item icons (native menus render them as template images). */
std::unique_ptr<juce::Drawable> shapeMenuIcon (Shape);
std::unique_ptr<juce::Drawable> placementMenuIcon (Placement);
/** A shape menu (item IDs = shape index + 1) with glyphs, current shape ticked. */
juce::PopupMenu makeShapeMenu (int currentShape);

/** Shape tab on the island's left edge (glyph + name); click opens the shape menu, wheel steps. */
class ShapeButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    ShapeButton (juce::RangedAudioParameter&, juce::UndoManager&, juce::Colour tint = {});
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::UndoManager& undo;
    juce::ParameterAttachment attachment;
    int index = 0;
    void setIndex (int);
};

/** Placement tab on the island's right edge (icon + band colour dot + name). */
class PlacementButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    PlacementButton (juce::RangedAudioParameter&, juce::UndoManager&, juce::Colour bandColour);
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::UndoManager& undo;
    juce::ParameterAttachment attachment;
    juce::Colour colour;
    int index = 0;
};

//==============================================================================
/** Icon button for the island: rests dim, fades up on hover, lights in its colour when on (with a soft glow),
    so state changes and hover feedback ease instead of snapping. */
class IslandButton : public juce::Button, private juce::Timer
{
public:
    IslandButton (st::icons::Icon, const juce::String& tooltip = {});
    void setOnColour (juce::Colour c) { onColour = c; repaint(); }
    void setMinimal (bool) {}
    /** Brighter and bolder at rest (the delete cross). */
    void setEmphasis (bool e) { emphasis = e; repaint(); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    void buttonStateChanged() override { startTimerHz (60); }
    void enablementChanged() override { startTimerHz (60); }

private:
    st::icons::Icon icon;
    juce::Colour onColour;
    bool emphasis = false;
    st::Animated hover, lit;
    void timerCallback() override;
};

//==============================================================================
/** The floating band island (Pro-style band controls).

    Body: a lens-shaped glass island whose top edge rises smoothly to a centre plateau. Left: power box, shape
    tab, slope. Centre: Freq, the recessed Gain (its outer ring sets the dynamic range: red span, live gain in
    yellow) and Q, with "0 dB" / "1" centre labels. Right: band nav, delete box, placement tab, split.

    Dynamic bands get a tab on the plateau (clear, dynamics on/off, spectral, expand). Expanding widens the island
    and adds the dynamics section between Gain and Q: auto button + threshold meter/slider, side chain,
    audition, Band / Free trigger and Attack / Release (+ Density and Tilt for spectral bands). All geometry is in island units (origin = left edge,
    y = 0 at the body's side top), placed inside the component with margins for the shadow. */
class BandPanel : public juce::Component, private juce::Timer
{
public:
    BandPanel (StEqProcessor&, int band, int numSelected, bool expanded);
    ~BandPanel() override;

    int getBand() const noexcept { return band; }
    bool isExpanded() const noexcept { return expanded; }
    void setExpanded (bool);

    /** Geometry is written in reference units measured from the original (island width 470 units) and drawn
        at kScale, which makes the island ~38 % of the window width like the reference. */
    static constexpr float kScale = 0.85f;
    static constexpr int collapsedWidth = 470, expandedExtra = 148, depthExtra = 74;   // reference units (depth: 3D mode)
    static constexpr float bodyHeight = 93.0f, bulge = 5.0f, tabTop = -20.0f, cornerRadius = 9.0f, tileSize = 24.0f;   // reference units
    static constexpr int marginX = 14, marginTop = 32, marginBottom = 16;          // component pixels
    static constexpr int height = marginTop + (int) ((bodyHeight + bulge) * kScale) + marginBottom;
    static constexpr float gainCentreX = 233.5f;
    int getIslandWidth() const noexcept { return juce::roundToInt (((float) collapsedWidth + shift()) * kScale); }
    int getPreferredWidth() const noexcept { return getIslandWidth() + 2 * marginX; }
    /** Component x of the gain knob centre (the island is placed so this sits under the band). */
    static constexpr int anchorX = marginX + (int) (gainCentreX * kScale);

    std::function<void()> onRemove, onPrevious, onNext, onSplit, onSizeChanged;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool hitTest (int x, int y) override { return outline.contains ((float) x, (float) y); }
    /** The island's outline (local coordinates): the display draws its frosted backdrop inside it. */
    const juce::Path& getOutline() const noexcept { return outline; }

private:
    StEqProcessor& proc;
    const int band, numSelected;
    bool expanded = false;

    std::unique_ptr<ShapeButton> shape;
    std::unique_ptr<PlacementButton> placement;
    std::unique_ptr<st::Dropdown> slope;
    std::unique_ptr<st::Knob> freq, gain, q, attack, release, density, depth;
    std::unique_ptr<juce::Component> threshold, autoBox, triggerButton;

    IslandButton bypassButton { st::icons::Icon::power, "Band on / off (Alt-click the dot)" };
    IslandButton closeButton { st::icons::Icon::close, "Delete band" };
    IslandButton prevButton { st::icons::Icon::chevronLeft, "Previous band" }, nextButton { st::icons::Icon::chevronRight, "Next band" };
    IslandButton gainQButton { st::icons::Icon::gears, "Gain-Q interaction: analog-style bells that narrow as the gain increases" };
    IslandButton splitButton { st::icons::Icon::scissors, "Split into left / right or mid / side" };
    // dynamics tab
    IslandButton clearDynButton { st::icons::Icon::close, "Clear dynamics" };
    IslandButton dynBypassButton { st::icons::Icon::power, "Dynamics on / off" };
    IslandButton spectralButton { st::icons::Icon::bars, "Spectral dynamics: each frequency in the band is compressed / expanded on its own (linear phase, adds latency)" };
    IslandButton tiltButton { st::icons::Icon::tilt, "Spectral tilt: +3 dB/oct detector tilt so high frequencies are judged fairly" };
    IslandButton expandButton { st::icons::Icon::chevronsRight, "Show dynamics settings" };
    IslandButton collapseButton { st::icons::Icon::chevronsLeft, "Hide dynamics settings" };
    IslandButton sidechainButton { st::icons::Icon::sidechain, "Trigger from the external side chain" };
    IslandButton auditionButton { st::icons::Icon::headphones, "Hold to hear what triggers the dynamics" };

    juce::Path outline, dynTab;
    juce::Image backgroundCache;
    float cacheScale = 0.0f;
    bool last3d = false;
    bool lastDynamic = false, lastBypass = false, lastDynBypass = false, lastGq = false, lastSc = false, lastSpectral = false, lastTilt = false;
    Shape lastShape = Shape::bell;
    int lastHoverMask = 0;

    juce::Point<float> toLocal (float x, float y) const noexcept { return { (float) marginX + x * kScale, (float) marginTop + y * kScale }; }
    static float fs (float referenceSize) noexcept { return referenceSize * kScale; }
    juce::Rectangle<int> box (float x, float y, float w, float h) const noexcept;
    /** Dynamics section width (moves Q) and, in 3D mode, the depth knob too (moves the right column). */
    float dynShift() const noexcept { return (float) expandedExtra * expAmt.current; }
    float shift() const noexcept { return dynShift() + (float) depthExtra * depthAmt.current; }
    /** x of the dynamics tab's right end. */
    float chevronX() const noexcept { return 276.0f + dynShift(); }
    float depthX() const noexcept { return 328.7f + dynShift() + 66.0f; }
    float attackX() const noexcept { return 331.0f - 4.0f * specAmt.current; }
    float releaseX() const noexcept { return 377.0f - 20.0f * specAmt.current; }
    st::Animated dynAmt, expAmt, specAmt, depthAmt;   // 0..1: dynamics tab present, dynamics section expanded, spectral, 3D mode (eased)
    bool animating = false;
    void animateStep (double dt);
    bool isDynamic() const;

    void timerCallback() override;
    void updateState();
    void buildOutline();
    /** Appends the dynamics tab's right edge (from just below the top edge, bowing out and into the gain well). */
    void appendTabEdge (juce::Path&) const;
    void paintBackground (juce::Graphics&);
    void paintLabel (juce::Graphics&, st::Knob&, const juce::String& label, juce::Point<float> centre);
};

} // namespace steq
