#pragma once
#include "../PluginProcessor.h"

namespace steq
{

/** Analyzer settings (editor-only, kept in uiState). */
struct AnalyzerSettings
{
    bool pre = true, post = true, sidechain = false;
    int rangeIndex = 1;          // 60 / 90 / 120 dB
    int speedIndex = 1;          // slow / medium / fast
    int tiltIndex = 2;           // 0 / 3 / 4.5 / 6 dB/oct
    bool freeze = false, grab = true;
    bool collisions = true;      // highlight where other instances' spectra collide with this one
    int resolutionIndex = 2;     // FFT 2048 / 4096 / 8192 / 16384
    static const char* resolutionNames[4];

    static const float ranges[3];
    static const float tilts[4];
    void load (const juce::ValueTree& ui);
    void save (juce::ValueTree& ui) const;
};

/** The full-window interactive EQ display.

    Draws (back to front): faded grid + scales, analyzer (pre faint, post warm, side chain cool), each band's
    filled area in its colour, the response curve(s), dynamic-range handles, band dots, the hover readout,
    rubber band, sketch stroke, spectrum-grab peaks and the output meter on the right edge.

    Interaction (FabFilter Pro-style): double-click / drag the curve to create bands, drag dots (Shift fine,
    Cmd = Q, Alt constrains), wheel = Q (cut slopes step), Alt+wheel = dynamic range, Cmd+wheel = gain,
    Alt-click = bypass, rubber-band and Cmd/Shift-click selection, right-click menus, Delete removes,
    Cmd-C / Cmd-V copy and paste, double-click a dot to type a frequency (Tab: gain, Q).

    3D mode (the instance's mode_3d parameter, on by default): every band also has a depth. Views (buttons top
    right, kept in uiState "view3d", Front by default; compact strips follow the instance): 3D (a raised, turned
    perspective camera you orbit with the axis gizmo; each band's curve stands at its depth, the total curve runs
    through the bands' depths, the analyser is a waterfall of recent spectra on the near floor; drag a dot for
    frequency / gain, its shadow on the floor for depth), Top (frequency across, depth up, the waterfall as
    ridgelines: drag a dot for frequency / depth) and Front (the normal display). */
class EqDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    /** compact: an embedded editor for another instance (instance list): no meter, scales, side buttons or
        range box; its analysers read the instance's spectrum without consuming it. */
    explicit EqDisplay (StEqProcessor&, bool compact = false);
    ~EqDisplay() override;

    // selection
    const juce::Array<int>& getSelection() const noexcept { return selection; }
    int getPrimary() const noexcept { return primary; }
    void select (int band, bool addToSelection = false);
    void setSelection (const juce::Array<int>&, int primaryBand);
    void removeSelected();
    /** Splits the selected stereo bands into left / right (or mid / side) pairs. */
    void splitSelected (bool midSide);
    std::function<void()> onSelectionChanged;
    std::function<void (double dt)> onFrame;

    // 3D mode
    enum class View { threeD, top, front };
    /** The view shown: the instance's choice while its 3D mode is on, else Front. */
    View getView() const;
    void setView (View);
    /** 3D view: screen position of (frequency, display dB, depth 0..1). */
    juce::Point<float> project (float hz, float db, float depth) const noexcept;
    /** 3D view: the floor point under a band's dot (drag it to change the depth). */
    juce::Point<float> depthHandle (int band) const;
    /** View buttons (3D, Top, Front), shown while 3D mode is on (not in compact rows). */
    juce::Rectangle<float> getViewButton (View) const;
    /** 3D view: the axis gizmo under the view buttons; drag it to turn / tilt the camera, double-click resets. */
    juce::Point<float> getGizmoCentre() const noexcept;
    /** 3D camera angles (radians; per instance, uiState "camYaw" / "camPitch"). */
    float getCameraYaw() const;
    float getCameraPitch() const;
    void setCameraAngles (float yaw, float pitch);
    static constexpr float defaultYaw = 0.15f, defaultPitch = 0.40f;

    // settings
    /** EQ scale. Auto (default): fits the bands and the curve with some headroom, between 3 and 30 dB; grows
        while a node is dragged past the edge and shrinks back once there's room (after the drag). Or fixed. */
    void setDisplayRange (int index);              // fixed: 0..3 -> 3 / 6 / 12 / 30 dB
    void setAutoRange();
    bool isAutoRange() const noexcept { return autoRange; }
    /** Current scale (+/- dB at the edge of the EQ area), animated. */
    float getDisplayRangeDb() const noexcept { return displayRange; }
    static const float displayRanges[4];
    static constexpr float minRange = 3.0f, maxRange = 30.0f;
    /** The scale the auto range aims for with the current bands (before animation). */
    float autoRangeTarget() const;
    /** Advances the range animation (normally every frame; tests call it directly). Returns true if it moved. */
    bool advanceRange (double seconds);
    /** Runs one UI frame (parameters, analyser, animations): the frame clock calls it; tests too. */
    void advanceFrame (double dt) { frame (dt); }
    void setAnalyzer (const AnalyzerSettings&);
    const AnalyzerSettings& getAnalyzer() const noexcept { return analyzer; }
    void setSketchMode (bool);
    bool isSketchMode() const noexcept { return sketchMode; }
    std::function<void (bool)> onSketchModeChanged;
    std::function<void (int)> onRangeChanged;

    /** 0 = pre, 1 = post, 2 = side chain. */
    st::SpectrumAnalyzer& getSpectrum (int which) noexcept { return which == 0 ? *preSpectrum : (which == 1 ? *postSpectrum : *scSpectrum); }
    std::function<void()> onMatchRequested;

    /** Plot geometry (local), for placing the band panel. */
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    float xForHz (float hz) const noexcept;
    float hzForX (float x) const noexcept;
    float getBandX (int band) const;
    float yForDb (float db) const noexcept;
    float dbForY (float y) const noexcept;
    juce::Point<float> dotPosition (int band) const;
    /** y of the (first) total curve at hz, from the curves as last built. */
    float getCurveY (float hz) const { return curveYAt (xForHz (hz)); }
    /** Re-reads the band parameters now (normally done every frame). */
    void syncFromParameters() { readParameters(); rebuildCurves(); }
    /** Spectrum (dB per analyser point, same mapping as getSpectrum(1)) of the other instances to show
        collisions against; null = none. The vector must outlive the display or be reset to null. */
    void setCollisionReference (const std::vector<float>* levels) { collisionRef = levels; }
    bool isCompact() const noexcept { return compact; }
    /** Parameter display of `band` (shown while hovering its dot): bounds of "freq", "gain", "q", "shape",
        "bypass", "solo" or "remove" (tests / tooling). */
    juce::Rectangle<float> getReadoutPart (int band, const juce::String& part) const;
    /** Shows the free trigger range bar of `band` at local y (band < 0 hides it). */
    void setTriggerBar (int band, float y) { triggerBand = band; triggerBarY = y; repaint(); }
    /** Frosted glass under the band island: inside `outline` (local coordinates) the display is drawn blurred,
        together with what lies behind it (`paintBackdrop`, in local coordinates), faded in by `alpha`. */
    void setFrost (const juce::Path& outline, float alpha);
    std::function<void (juce::Graphics&)> paintBackdrop;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    StEqProcessor& proc;
    const bool compact;
    const std::vector<float>* collisionRef = nullptr;
    std::vector<float> maxHold;   // Freeze: maximum of the post spectrum since freeze was switched on
    void paintCollisions (juce::Graphics&, juce::Rectangle<float> area, float range);
    std::unique_ptr<st::SpectrumAnalyzer> preSpectrum, postSpectrum, scSpectrum;
    AnalyzerSettings analyzer;
    std::unique_ptr<st::FrameClock> clock;
    st::IconButton sketchButton { st::icons::Icon::pencil, "EQ Sketch: draw the curve you want, bands are created to match" };
    st::IconButton pianoButton { st::icons::Icon::bars, "Piano display: shows a keyboard and snaps band frequencies to notes (Shift-drag to bypass)" };
    bool piano = false;
    void paintPiano (juce::Graphics&);

    juce::Rectangle<float> plot, eqArea, meterArea;
    std::unique_ptr<juce::Component> rangeBox;
    int triggerBand = -1;          // free-trigger range bar (shown while the band's dynamics are expanded)
    float triggerBarY = -1.0f;
    int rangeIndex = 2;              // fixed range choice
    bool autoRange = true, rangeReady = false;
    float displayRange = 12.0f, rangeTarget = 12.0f;
    float dragDbAccum = 0.0f, lastDragY = 0.0f;   // band drags accumulate dB step by step (stable while the scale moves)
    juce::Point<float> lastDragPos;               // 3D view: the previous drag point (dB steps on the band's plane)
    void rangeChanged();
    static constexpr float minHz = 10.0f, maxHz = 30000.0f;

    // --- band state cache (read every frame from the parameters) ---------------------------------
    std::array<BandParams, kMaxBands> bands {};
    float gainScale = 1.0f;
    int procMode = 0;
    double sampleRate = 48000.0;
    std::array<float, kMaxBands> liveDyn {};
    std::array<std::array<float, SpectralDynamics::kGrid>, kMaxBands> specAmount {};   // spectral bands: live amount per grid frequency
    std::array<int, kMaxBands> specVersion {};

    // --- curves ----------------------------------------------------------------------------------
    static constexpr int kPoints = 360;
    std::array<float, kPoints> freqs {}, xs {};
    struct Curve
    {
        std::array<float, kPoints> db {}, rangeDb {}, liveDb {};   // static, static + dynamic range, live (with dynamics)
        BandParams key; float scale = 0.0f, dyn = 0.0f; int mode = -1, specVersion = -1; double sr = 0.0; bool valid = false, hasRange = false;
    };
    std::array<Curve, kMaxBands> curves;
    std::array<std::array<float, kPoints>, 2> totals {};
    int totalsCount = 1;            // 1 = single stereo curve, 2 = two channel curves
    juce::String totalsLabels[2];
    bool curvesDirty = true;
    juce::Image curveLayer, gridLayer;
    float layerScale = 0.0f;
    bool gridDirty = true;

    // --- animation ---------------------------------------------------------------------------------
    std::array<st::Animated, kMaxBands> hoverAmt, selectAmt;
    st::Animated previewAlpha, sketchAlpha;
    double sinceRepaint = 0.0;

    // --- interaction -----------------------------------------------------------------------------
    juce::Array<int> selection;
    int primary = -1;
    int hoverBand = -1, hoverDynBand = -1;
    juce::Point<float> mousePos { -100.0f, -100.0f };
    bool mouseInside = false;

    enum class Drag { none, bands, dynRange, rubber, sketch, pendingCurve, pendingGrab, triggerLo, triggerHi, value, solo, depth, orbit };

    // --- parameter display (next to the hovered dot): live values you can drag / wheel / double-click, and quick
    //     buttons (shape, bypass, solo: click and hold, drag to move the band while listening, delete) ---------
    enum class Part { none, freq, gain, q, shape, bypass, solo, remove };
    struct Readout
    {
        juce::Rectangle<float> box, buttons;
        std::array<juce::Rectangle<float>, 4> values {};   // freq, gain, q, range
        std::array<Part, 4> parts { Part::none, Part::none, Part::none, Part::none };
        juce::Rectangle<float> shape, bypass, solo, remove;
        int lines = 0;
    };
    Readout readoutLayout (int band) const;
    Part partAt (juce::Point<float>) const;
    int readoutBand = -1;
    Part hoverPart = Part::none, valuePart = Part::none;
    float valueStart = 0.0f, valueStart2 = 0.0f;
    int lastSoloBand = -1;
    Drag drag = Drag::none;
    juce::Point<float> dragStart;
    struct Start { int band; float freq, gain, q, range; BandParams params; };
    std::vector<Start> dragStarts;
    juce::Rectangle<float> rubber;
    juce::Array<int> rubberBase;
    bool axisLocked = false, lockHorizontal = true;
    int pendingAltClick = -1;          // band to bypass if an Alt-click ends without a drag
    bool pendingDynamic = false, pendingSpectral = false;
    float pendingGrabHz = 0.0f;
    std::vector<juce::Point<float>> sketchPoints;
    // progressive sketch: bands are fitted while drawing, on top of the curve as it was when the stroke began
    std::array<float, kPoints> sketchBase {};
    juce::Array<int> sketchBands;
    double lastSketchFit = 0.0;
    void refitSketch();
    bool sketchMode = false;

    // spectrum grab
    struct Peak { float hz, db, x, y; };
    std::vector<Peak> peaks;
    int hoverPeak = -1;
    bool grabFrozen = false;

    // output meter
    std::array<float, 2> meterDb { -100.0f, -100.0f }, meterHold { -100.0f, -100.0f };
    float meterMax = -100.0f;
    std::array<double, 2> holdTime {};

    // value entry
    std::unique_ptr<juce::TextEditor> entry, retiredEntry;
    int entryBand = -1, entryField = 0;

    // --- 3D mode ---------------------------------------------------------------------------------------
    bool mode3d = false;
    int hoverDepthBand = -1;
    juce::Point<float> depthGrabOffset;
    bool hoverGizmo = false;
    float orbitYaw0 = 0.0f, orbitPitch0 = 0.0f;
    bool gizmoAt (juce::Point<float>) const noexcept;
    bool flatView() const { return getView() == View::front; }
    // 3D camera: a raised, turned perspective view of the box (frequency x gain x depth), fitted to the plot
    struct Camera
    {
        juce::Rectangle<float> key; float keyYaw = 0, keyPitch = 0; bool valid = false;
        float cy = 1, sy = 0, cp = 1, sp = 0;     // yaw / pitch
        float kx = 1, ky = 1, ox = 0, oy = 0;     // fit to the plot
        juce::Point<float> raw (float X, float Y, float Z) const noexcept;
        /** Inverse of raw on the plane at Z: the X, Y under a raw screen point (false if the plane is edge-on). */
        bool unproject (juce::Point<float> r, float Z, float& X, float& Y) const noexcept;
    };
    mutable Camera camera;
    const Camera& getCamera() const noexcept;
    juce::Point<float> projectUnit (float u, float v, float depth) const noexcept;   // u: 0..1 frequency, v: -1..1 gain
    /** 3D: the (frequency, dB) on the vertical plane at a depth under a screen point. */
    bool planePoint (juce::Point<float>, float depth, float& hz, float& db) const noexcept;
    /** 3D: the depth whose floor point along a frequency's floor line is nearest a screen point. */
    float depthAlongFloor (float hz, juce::Point<float>) const noexcept;
    /** 3D: how big things look at a depth relative to the front (perspective). */
    float depthScale (float depth) const noexcept;
    // 3D analyser: a waterfall of recent spectra receding from the front over the floor
    static constexpr int kWaterfall = 32, kWaterfall3D = 14;   // history kept; slices the 3D view shows (Top shows all)
    std::array<std::vector<float>, kWaterfall> waterfall;
    std::vector<float> waterfallHz, preHz;
    int waterfallHead = 0, waterfallCount = 0;
    double waterfallClock = 0.0;
    bool pushWaterfall (double dt);
    juce::Rectangle<float> topArea() const noexcept;
    float topY (float depth) const noexcept;
    float topDepth (float y) const noexcept;
    int depthHandleAt (juce::Point<float>) const;
    int viewButtonAt (juce::Point<float>) const;
    /** Depth (0..1) of the total curve at each point: the bands' depths weighted by how much each one acts there. */
    void curveDepths (std::array<float, kPoints>&) const;
    /** Screen point for a click in the 3D / Top view, as the same band would sit in the Front view (+ its depth). */
    juce::Point<float> frontPointFor (juce::Point<float>, float& depth) const;
    void paint3D (juce::Graphics&);
    void paintTop (juce::Graphics&);
    void paintViewButtons (juce::Graphics&);

    // --- helpers -----------------------------------------------------------------------------------
    float analyzerY (float db) const noexcept;
    float dotDb (const BandParams&) const noexcept;          // display dB of the band's dot
    void applyVertical (int band, const BandParams& start, float dDb);   // vertical drag by dDb (display dB)
    float curveYAt (float x) const;
    int bandAt (juce::Point<float>) const;
    int dynHandleAt (juce::Point<float>) const;
    juce::Rectangle<float> dynHandleBounds (int band) const;

    void frame (double dt);
    bool readParameters();
    void rebuildCurves();
    void computeBandCurve (int band, Curve&);
    void rebuildLayers (float pixelScale);
    void paintGrid (juce::Graphics&);
    void paintCurves (juce::Graphics&);
    void paintSpectrum (juce::Graphics&);
    void paintFrost (juce::Graphics&);
    juce::Path frostOutline;
    float frostAlpha = 0.0f;
    juce::Image frostImage;
    juce::Rectangle<int> frostArea;
    int frostFrame = 0;
    void paintDots (juce::Graphics&);
    void paintDynHandles (juce::Graphics&);
    void paintPreview (juce::Graphics&);
    void paintReadout (juce::Graphics&);
    void paintMeter (juce::Graphics&);
    void paintGrab (juce::Graphics&);
    void paintTriggerBar (juce::Graphics&);
    int triggerHandleAt (juce::Point<float>) const;
    void updatePeaks();
    void createAnalyzers();

    // actions
    int createBandAt (juce::Point<float>, bool fromCurveDrag, bool dynamic, bool spectral = false);
    Shape shapeForPosition (juce::Point<float>, bool fromCurveDrag) const;
    void beginBandDrag (const juce::MouseEvent&);
    void applyBandDrag (const juce::MouseEvent&);
    void endGestures();
    void showBandMenu (int band);
    void showBackgroundMenu();
    void showEntry (int band, int field);
    void commitEntry (bool moveNext);
    void finishSketch();
    void notifySelection();

    juce::Array<int> gestureBands;
    void beginGesture (int band, const char* suffix);
    void setPlain (int band, const char* suffix, float v);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqDisplay)
};

/** "1.25 kHz", "440 Hz" */
juce::String formatFrequency (float hz);
/** 0..1: how strongly two spectra (analyser dB) collide at one frequency (both loud). */
float collisionAmount (float ownDb, float otherDb) noexcept;
/** Note name with cents, e.g. "A4", "C#2 +13". */
juce::String noteName (float hz);
/** Parses "2k", "1.5 kHz", "440", note names ("A4", "C#2+13"). Returns < 0 on failure. */
float parseFrequency (const juce::String&);

} // namespace steq
