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
    Cmd-C / Cmd-V copy and paste, double-click a dot to type a frequency (Tab: gain, Q). */
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

    // settings
    void setDisplayRange (int index);              // 0..3 -> 3 / 6 / 12 / 30 dB
    int getDisplayRange() const noexcept { return rangeIndex; }
    static const float displayRanges[4];
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
    int rangeIndex = 2;
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

    enum class Drag { none, bands, dynRange, rubber, sketch, pendingCurve, pendingGrab, triggerLo, triggerHi, value, solo };

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
