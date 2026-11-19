#include "Panels.h"
#include "../dsp/Sketch.h"

namespace steq
{

//==============================================================================
MatchPopover::MatchPopover (StEqProcessor& p, EqDisplay& d, InstanceHub& h)
    : st::Popover ("EQ Match"), proc (p), display (d), hub (h)
{
    savedAnalyzer = display.getAnalyzer();
    auto a = savedAnalyzer;
    a.pre = true; a.sidechain = true; a.freeze = false;
    display.setAnalyzer (a);

    learn.setClickingTogglesState (true);
    learn.setOnColour (st::Theme::get().accent2);
    learn.setTooltip ("Play the reference into the side chain and your track through the plug-in, then stop");
    learn.onClick = [this]
    {
        learning = learn.getToggleState();
        learn.setButtonText (learning ? "Stop" : "Learn");
        if (! learning) computeTarget();
    };
    clear.onClick = [this]
    {
        frames = 0;
        std::fill (curSum.begin(), curSum.end(), 0.0);
        std::fill (refSum.begin(), refSum.end(), 0.0);
        target.clear();
        repaint();
    };
    apply.setTooltip ("Create bands that match the input to the reference");
    apply.onClick = [this] { applyMatch(); };
    bandsButton.setDropdownArrow (true);
    bandsButton.onClick = [this]
    {
        juce::PopupMenu m;
        for (int n : { 2, 4, 6, 8, 12 }) m.addItem (n, juce::String (n) + " bands", true, n == maxBands);
        st::showMenu (m, &bandsButton, [safe = juce::Component::SafePointer<MatchPopover> (this)] (int r)
        {
            if (safe != nullptr && r > 0) { safe->maxBands = r; safe->updateButtons(); }
        });
    };
    sourceButton.setDropdownArrow (true);
    sourceButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (juce::PopupMenu::Item ("Side Chain").setTicked (reference.get() == nullptr).setAction ([this] { reference = nullptr; updateButtons(); }));
        if (! hub.getOthers().empty()) m.addSeparator();
        for (auto& e : hub.getOthers())
        {
            st::ProcessorBase* ip = e->proc;
            m.addItem (juce::PopupMenu::Item (e->proc->getInstanceLabel()).setTicked (reference.get() == ip)
                           .setAction ([this, ip] { reference = ip; updateButtons(); }));
        }
        st::showMenu (m, &sourceButton);
    };
    detailButton.setDropdownArrow (true);
    detailButton.onClick = [this]
    {
        juce::PopupMenu m;
        const char* names[] = { "Very Low (1 oct)", "Low (1/2 oct)", "Medium (1/3 oct)", "High (1/6 oct)", "Very High (1/12 oct)" };
        for (int i = 0; i < 5; ++i)
            m.addItem (juce::PopupMenu::Item (names[i]).setTicked (detailIndex == i).setAction ([this, i] { detailIndex = i; updateButtons(); computeTarget(); repaint(); }));
        st::showMenu (m, &detailButton);
    };
    amountButton.setDropdownArrow (true);
    amountButton.onClick = [this]
    {
        juce::PopupMenu m;
        for (int pct : { 25, 50, 75, 100 })
            m.addItem (juce::PopupMenu::Item (juce::String (pct) + "%").setTicked (amountPct == pct).setAction ([this, pct] { amountPct = pct; updateButtons(); computeTarget(); repaint(); }));
        st::showMenu (m, &amountButton);
    };
    updateButtons();

    for (juce::Component* c : { (juce::Component*) &learn, (juce::Component*) &clear, (juce::Component*) &apply, (juce::Component*) &bandsButton,
                                (juce::Component*) &sourceButton, (juce::Component*) &detailButton, (juce::Component*) &amountButton })
        addAndMakeVisible (c);
    startTimerHz (20);
}

void MatchPopover::updateButtons()
{
    bandsButton.setButtonText (juce::String (maxBands));
    bandsButton.setDimmedText ("Bands  ");
    sourceButton.setButtonText (reference.get() != nullptr ? reference->getInstanceLabel() : juce::String ("Side Chain"));
    sourceButton.setDimmedText ("Reference  ");
    const char* short_[] = { "1 oct", "1/2 oct", "1/3 oct", "1/6 oct", "1/12 oct" };
    detailButton.setButtonText (short_[detailIndex]);
    detailButton.setDimmedText ("Detail  ");
    amountButton.setButtonText (juce::String (amountPct) + "%");
    amountButton.setDimmedText ("Amount  ");
}

const std::vector<float>* MatchPopover::referenceLevels() const
{
    if (reference.get() == nullptr) return &display.getSpectrum (2).getLevelsDb();
    for (auto& e : hub.getOthers())
        if (e->proc == reference.get() && e->spectrum != nullptr) return &e->spectrum->getLevelsDb();
    return nullptr;
}

MatchPopover::~MatchPopover()
{
    stopTimer();
    display.setAnalyzer (savedAnalyzer);
}

void MatchPopover::timerCallback()
{
    if (! learning) { repaint (graph.toNearestInt().expanded (2)); return; }
    const auto& cur = display.getSpectrum (0).getLevelsDb();
    const auto* refPtr = referenceLevels();
    if (refPtr == nullptr || refPtr->size() != cur.size()) return;
    const auto& ref = *refPtr;
    if (curSum.size() != cur.size()) { curSum.assign (cur.size(), 0.0); refSum.assign (cur.size(), 0.0); frames = 0; }

    // only frames where both signals are present
    float curMax = -200.0f, refMax = -200.0f;
    for (size_t i = 0; i < cur.size(); ++i) { curMax = juce::jmax (curMax, cur[i]); refMax = juce::jmax (refMax, ref[i]); }
    if (curMax < -90.0f || refMax < -90.0f) return;

    for (size_t i = 0; i < cur.size(); ++i) { curSum[i] += cur[i]; refSum[i] += ref[i]; }
    ++frames;
    if (frames % 10 == 0) computeTarget();
    repaint (graph.toNearestInt().expanded (2));
}

void MatchPopover::computeTarget()
{
    if (frames < 5) { target.clear(); return; }
    auto& an = display.getSpectrum (0);
    const int n = (int) curSum.size();
    std::vector<float> diff ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) diff[(size_t) i] = (float) ((refSum[(size_t) i] - curSum[(size_t) i]) / frames);

    // 1/3-octave smoothing
    std::vector<float> sm ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const float f = an.getFrequencyForPoint (i);
        double sum = 0.0; int cnt = 0;
        for (int k = 0; k < n; ++k)
        {
            const float fk = an.getFrequencyForPoint (k);
            if (std::abs (std::log2 (fk / f)) <= detailOctaves[detailIndex] * 0.5f) { sum += diff[(size_t) k]; ++cnt; }
        }
        sm[(size_t) i] = cnt > 0 ? (float) (sum / cnt) : 0.0f;
    }
    // remove the level difference (tonal match only)
    double mean = 0.0; int cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        const float f = an.getFrequencyForPoint (i);
        if (f > 100.0f && f < 8000.0f) { mean += sm[(size_t) i]; ++cnt; }
    }
    mean = cnt > 0 ? mean / cnt : 0.0;
    target.assign ((size_t) n, 0.0f);
    const float amount = (float) amountPct * 0.01f;
    for (int i = 0; i < n; ++i) target[(size_t) i] = juce::jlimit (-12.0f, 12.0f, (float) (sm[(size_t) i] - mean) * amount);
}

void MatchPopover::applyMatch()
{
    if (learning) { learn.setToggleState (false, juce::sendNotification); }
    computeTarget();
    if (target.empty()) return;

    auto& an = display.getSpectrum (0);
    std::vector<std::pair<float, float>> pts;
    for (int i = 0; i < (int) target.size(); ++i)
    {
        const float f = an.getFrequencyForPoint (i);
        if (f >= 30.0f && f <= 16000.0f) pts.push_back ({ f, target[(size_t) i] });
    }
    int free = 0;
    for (int b = 0; b < kMaxBands; ++b) free += proc.getBand (b).used ? 0 : 1;
    const auto fitted = sketch::fit (pts, juce::jmin (maxBands, free), 0.75f);

    proc.undoManager.beginNewTransaction ("EQ Match");
    juce::Array<int> added;
    for (auto& f : fitted)
    {
        const int b = proc.addBand (f.shape, f.freq, f.gain, f.q, f.slope, Placement::stereo, false);
        if (b >= 0) added.add (b);
    }
    if (! added.isEmpty()) display.setSelection (added, added.getLast());
}

void MatchPopover::layoutContent (juce::Rectangle<int> r)
{
    auto row = r.removeFromBottom (30);
    learn.setBounds (row.removeFromLeft (86).reduced (0, 3));
    row.removeFromLeft (8);
    clear.setBounds (row.removeFromLeft (70).reduced (0, 3));
    apply.setBounds (row.removeFromRight (86).reduced (0, 3));
    row.removeFromRight (8);
    bandsButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (4);
    amountButton.setBounds (row.removeFromRight (100));
    row.removeFromRight (4);
    detailButton.setBounds (row.removeFromRight (110));
    auto top = getHeaderBounds();
    sourceButton.setBounds (top.removeFromRight (200));
    r.removeFromBottom (8);
    graph = r.toFloat();
}

void MatchPopover::paintContent (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    g.setColour (juce::Colours::black.withAlpha (0.32f));
    g.fillRoundedRectangle (graph, 6.0f);
    auto area = graph.reduced (8.0f, 8.0f);
    auto& an = display.getSpectrum (0);
    const int n = (int) curSum.size();

    auto xFor = [&] (float f) { return area.getX() + area.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
    for (float f : { 100.0f, 1000.0f, 10000.0f })
        st::paint::fadedLine (g, { xFor (f), area.getY() }, { xFor (f), area.getBottom() }, juce::Colours::white.withAlpha (0.06f), 1.0f, 0.15f);
    st::paint::fadedLine (g, { area.getX(), area.getCentreY() }, { area.getRight(), area.getCentreY() }, juce::Colours::white.withAlpha (0.1f), 1.0f, 0.05f);

    auto drawCurve = [&] (std::function<float (int)> value, float lo, float hi, juce::Colour c, float w)
    {
        juce::Path p;
        bool started = false;
        for (int i = 0; i < n; ++i)
        {
            const float f = an.getFrequencyForPoint (i);
            if (f < 20.0f || f > 20000.0f) continue;
            const float y = juce::jmap (juce::jlimit (lo, hi, value (i)), lo, hi, area.getBottom(), area.getY());
            if (! started) { p.startNewSubPath (xFor (f), y); started = true; } else p.lineTo (xFor (f), y);
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved));
    };

    if (frames > 0)
    {
        drawCurve ([&] (int i) { return (float) (curSum[(size_t) i] / frames); }, -100.0f, 0.0f, t.spectrum.withAlpha (0.6f), 1.0f);
        drawCurve ([&] (int i) { return (float) (refSum[(size_t) i] / frames); }, -100.0f, 0.0f, t.seriesColour (1).withAlpha (0.6f), 1.0f);
    }
    if (! target.empty())
        drawCurve ([&] (int i) { return target[(size_t) i]; }, -15.0f, 15.0f, t.curve, 2.0f);

    g.setFont (st::fonts::medium (11.0f));
    g.setColour (t.textFaint);
    juce::String hint;
    if (reference.get() == nullptr && ! proc.hasSidechain()) hint = "Route the reference to the side chain, or pick another ST EQ instance as the reference";
    else if (frames == 0) hint = learning ? "Listening... play both signals" : "Press Learn and play both signals";
    else hint = juce::String (frames / 20) + " s learned";
    g.drawText (hint, graph.reduced (12.0f, 8.0f), juce::Justification::topLeft, false);
    g.setColour (t.spectrum.withAlpha (0.8f));
    g.drawText ("Input", graph.reduced (12.0f, 8.0f), juce::Justification::topRight, false);
    g.setColour (t.seriesColour (1).withAlpha (0.8f));
    g.drawText ("Reference", graph.reduced (12.0f, 8.0f).translated (0.0f, 14.0f), juce::Justification::topRight, false);
    g.setColour (t.curve);
    g.drawText ("Match", graph.reduced (12.0f, 8.0f).translated (0.0f, 28.0f), juce::Justification::topRight, false);
}

//==============================================================================
OutputPanel::OutputPanel (StEqProcessor& p)
    : st::Popover ("Output"),
      output (p.param (ids::outGain), &p.undoManager, "Out"),
      scale (p.param (ids::gainScale), &p.undoManager, "Scale"),
      panMode (p.param (ids::panMode), &p.undoManager),
      autoGain (p.param (ids::autoGain), &p.undoManager, "Auto Gain", st::Theme::get().positive),
      phase (p.param (ids::phaseInv), &p.undoManager, juce::String (juce::CharPointer_UTF8 ("\xc3\x98")) + " Polarity", st::Theme::get().accent2)
{
    const auto& t = st::Theme::get();
    output.setSize (st::Knob::Size::medium);
    output.setLook (st::Knob::Look::arc);
    output.setColour (t.curve);
    output.setBipolar (true);
    output.setRecessed (true);
    output.setRimParameter (&p.param (ids::outPan), "Pan", true);
    output.setTooltip ("Output gain. The outer ring is the pan / balance (drag it).");
    scale.setSize (st::Knob::Size::small);
    scale.setLook (st::Knob::Look::arc);
    scale.setColour (t.curve);
    scale.setTooltip ("Gain scale: scales the gain and dynamic range of every band (100 % = as set, negative inverts)");
    panMode.setFlatStyle (true);
    panMode.setCaption ("Pan");
    panMode.setTooltip ("Pan mode: left / right balance, or mid / side balance");
    autoGain.setTooltip ("Compensates the overall level change of the EQ curve");
    phase.setTooltip ("Invert polarity");
    for (juce::Component* c : { (juce::Component*) &output, (juce::Component*) &scale, (juce::Component*) &panMode,
                                (juce::Component*) &autoGain, (juce::Component*) &phase })
        addAndMakeVisible (c);
}

void OutputPanel::layoutContent (juce::Rectangle<int> r)
{
    auto left = r.removeFromLeft (150);
    output.setBounds (left.withSizeKeepingCentre (128, 150));
    r.removeFromLeft (10);
    auto top = r.removeFromTop (92);
    scale.setBounds (top.withSizeKeepingCentre (80, 92));
    r.removeFromTop (4);
    panMode.setBounds (r.removeFromTop (24));
    r.removeFromTop (4);
    auto row = r.removeFromTop (26);
    autoGain.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
    phase.setBounds (row.reduced (2, 0));
}

void OutputPanel::paintContent (juce::Graphics& g)
{
    // a faint divider between the output knob and the other settings
    auto r = getContentBounds().toFloat();
    st::paint::fadedLine (g, { r.getX() + 155.0f, r.getY() + 6.0f }, { r.getX() + 155.0f, r.getBottom() - 6.0f },
                          juce::Colours::white.withAlpha (0.08f), 1.0f, 0.4f);
}

//==============================================================================
FooterBar::FooterBar (StEqProcessor& p)
    : proc (p),
      mode (p.param (ids::mode), &p.undoManager),
      resolution (p.param (ids::resolution), &p.undoManager),
      character (p.param (ids::character), &p.undoManager),
      bypassAttachment (p.param (ids::bypass), bypass, &p.undoManager)
{
    mode.setFlatStyle (true);
    resolution.setFlatStyle (true);
    resolution.setCaption ("Res");
    character.setFlatStyle (true);
    character.setCaption ("Character");
    bypass.setMinimal (true);
    bypass.setClickingTogglesState (true);
    bypass.setOnColour (st::Theme::get().warning);

    mode.setTooltip ("Zero Latency: analog-matched IIR. Natural Phase: analog magnitude and phase (small latency). Linear Phase: no phase shift (latency).");
    resolution.setTooltip ("Processing resolution (linear phase FIR length, spectral dynamics FFT size): higher = more accurate low end, more latency");
    character.setTooltip ("Clean, or subtle / warm analog-style saturation");

    analyzer.load (proc.uiState);
    analyzerButton.setDropdownArrow (true);
    analyzerButton.onClick = [this] { showAnalyzerMenu(); };
    instancesButton.setDropdownArrow (true);
    instancesButton.onClick = [this] { if (onInstancesRequested) onInstancesRequested(); };
    outputButton.setDropdownArrow (true);
    outputButton.onClick = [this] { if (onOutputRequested) onOutputRequested(); };

    for (juce::Component* c : { (juce::Component*) &mode, (juce::Component*) &resolution, (juce::Component*) &character,
                                (juce::Component*) &analyzerButton, (juce::Component*) &instancesButton, (juce::Component*) &outputButton,
                                (juce::Component*) &bypass })
        addAndMakeVisible (c);

    setAnalyzer (analyzer);
    timerCallback();
    startTimerHz (8);
}

FooterBar::~FooterBar() { stopTimer(); }

void FooterBar::timerCallback()
{
    // resolution matters for linear phase and for spectral dynamics (its FFT size)
    const bool linear = proc.refs.mode.getInt() == (int) ProcMode::linearPhase || proc.anySpectralBand();
    bool relayout = false;
    if (linear != lastLinear || resolution.isVisible() != linear)
    {
        lastLinear = linear;
        resolution.setVisible (linear);
        relayout = true;
    }

    // centre: this instance's track name and how many instances there are
    const int n = st::InstanceRegistry::get().getNumInstances();
    const auto label = proc.getInstanceLabel();
    const auto inst = label + "|" + juce::String (n);
    if (inst != lastInstances)
    {
        lastInstances = inst;
        instancesButton.setButtonText (label);
        instancesButton.setDimmedText ({}, n > 1 ? "  " + juce::String (n) : juce::String());
    }

    // output: gain (A when auto gain is on)
    const float out = proc.refs.outGain.get();
    auto text = (std::abs (out) < 0.005f ? juce::String ("0.0") : juce::String (out, 1)) + " dB";
    if (proc.refs.autoGain.getBool()) text << "  A";
    if (text != lastOutput)
    {
        lastOutput = text;
        outputButton.setButtonText (text);
        outputButton.setDimmedText ("Out  ");
    }

    if (relayout) { resized(); repaint(); }
}

void FooterBar::setAnalyzer (const AnalyzerSettings& a)
{
    analyzer = a;
    juce::String what;
    if (! a.pre && ! a.post && ! a.sidechain) what = "Off";
    else
    {
        juce::StringArray parts;
        if (a.pre) parts.add ("Pre");
        if (a.post) parts.add ("Post");
        if (a.sidechain) parts.add ("SC");
        what = parts.joinIntoString (" + ");
    }
    analyzerButton.setButtonText (what);
    analyzerButton.setDimmedText ("Analyzer  ");
}

void FooterBar::showAnalyzerMenu()
{
    auto a = analyzer;
    juce::PopupMenu m, rangeM, speedM, tiltM, resM;
    auto apply = [safe = juce::Component::SafePointer<FooterBar> (this)] (AnalyzerSettings s)
    {
        if (safe == nullptr) return;
        safe->setAnalyzer (s);
        if (safe->onAnalyzerChanged) safe->onAnalyzerChanged (s);
    };

    m.addItem (juce::PopupMenu::Item ("Pre-EQ").setTicked (a.pre).setAction ([a, apply] { auto s = a; s.pre = ! s.pre; apply (s); }));
    m.addItem (juce::PopupMenu::Item ("Post-EQ").setTicked (a.post).setAction ([a, apply] { auto s = a; s.post = ! s.post; apply (s); }));
    m.addItem (juce::PopupMenu::Item ("Side Chain").setTicked (a.sidechain).setAction ([a, apply] { auto s = a; s.sidechain = ! s.sidechain; apply (s); }));
    m.addSeparator();
    for (int i = 0; i < 3; ++i)
        rangeM.addItem (juce::PopupMenu::Item (juce::String ((int) AnalyzerSettings::ranges[i]) + " dB").setTicked (a.rangeIndex == i)
                            .setAction ([a, apply, i] { auto s = a; s.rangeIndex = i; apply (s); }));
    const char* speeds[] = { "Slow", "Medium", "Fast" };
    for (int i = 0; i < 3; ++i)
        speedM.addItem (juce::PopupMenu::Item (speeds[i]).setTicked (a.speedIndex == i).setAction ([a, apply, i] { auto s = a; s.speedIndex = i; apply (s); }));
    for (int i = 0; i < 4; ++i)
        tiltM.addItem (juce::PopupMenu::Item (juce::String (AnalyzerSettings::tilts[i], i == 2 ? 1 : 0) + " dB/oct").setTicked (a.tiltIndex == i)
                           .setAction ([a, apply, i] { auto s = a; s.tiltIndex = i; apply (s); }));
    for (int i = 0; i < 4; ++i)
        resM.addItem (juce::PopupMenu::Item (AnalyzerSettings::resolutionNames[i]).setTicked (a.resolutionIndex == i)
                          .setAction ([a, apply, i] { auto s = a; s.resolutionIndex = i; apply (s); }));
    m.addSubMenu ("Range", rangeM);
    m.addSubMenu ("Resolution", resM);
    m.addSubMenu ("Speed", speedM);
    m.addSubMenu ("Tilt", tiltM);
    m.addSeparator();
    m.addItem (juce::PopupMenu::Item ("Freeze (build maximum)").setTicked (a.freeze).setAction ([a, apply] { auto s = a; s.freeze = ! s.freeze; apply (s); }));
    m.addItem (juce::PopupMenu::Item ("Spectrum Grab").setTicked (a.grab).setAction ([a, apply] { auto s = a; s.grab = ! s.grab; apply (s); }));
    m.addItem (juce::PopupMenu::Item ("Show Collisions").setTicked (a.collisions).setAction ([a, apply] { auto s = a; s.collisions = ! s.collisions; apply (s); }));
    m.addSeparator();
    m.addItem (juce::PopupMenu::Item ("EQ Match...").setAction ([safe = juce::Component::SafePointer<FooterBar> (this)] { if (safe != nullptr && safe->onMatchRequested) safe->onMatchRequested(); }));
    st::showMenu (m, &analyzerButton);
}

void FooterBar::paint (juce::Graphics& g)
{
    // flat dark bottom bar with a hairline on top (Pro-style)
    const auto& t = st::Theme::get();
    auto r = getLocalBounds().toFloat();
    g.setColour (t.bg0.withAlpha (0.92f));
    g.fillRect (r);
    st::paint::fadedLine (g, { r.getX(), r.getY() + 0.5f }, { r.getRight(), r.getY() + 0.5f }, juce::Colours::white.withAlpha (0.07f), 1.0f, 0.04f);
    for (float x : dividers)
        st::paint::fadedLine (g, { x + 0.5f, r.getY() + 8.0f }, { x + 0.5f, r.getBottom() - 8.0f }, juce::Colours::white.withAlpha (0.08f), 1.0f, 0.5f);
}

void FooterBar::resized()
{
    auto r = getLocalBounds().reduced (24, 0);
    dividers.clear();

    // left: processing
    mode.setBounds (r.removeFromLeft (122));
    if (resolution.isVisible()) resolution.setBounds (r.removeFromLeft (112));

    // right: analyser | character | bypass | output
    outputButton.setBounds (r.removeFromRight (118));
    r.removeFromRight (4);
    bypass.setBounds (r.removeFromRight (30));
    dividers.push_back ((float) bypass.getX() - 10.0f);
    r.removeFromRight (20);
    character.setBounds (r.removeFromRight (142));
    dividers.push_back ((float) character.getX() - 10.0f);
    r.removeFromRight (20);
    analyzerButton.setBounds (r.removeFromRight (170));

    // centre: instances (centred on the bar when there is room)
    const int w = 200;
    auto centre = juce::Rectangle<int> (w, r.getHeight()).withCentre ({ getWidth() / 2, r.getCentreY() });
    if (centre.getX() < r.getX()) centre.setX (r.getX());
    if (centre.getRight() > r.getRight()) centre.setRight (r.getRight());
    instancesButton.setBounds (centre);
}

} // namespace steq
