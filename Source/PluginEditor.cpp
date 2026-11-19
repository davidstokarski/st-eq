#include "PluginEditor.h"

using namespace steq;

StEqEditor::StEqEditor (StEqProcessor& p)
    : EditorBase (p, designWidth, designHeight), proc (p), display (p), footer (p), hub (p)
{
    header.setProductName ("EQ");

    auto& content = getContent();
    content.addAndMakeVisible (display);
    content.addAndMakeVisible (footer);
    footer.setBufferedToImage (true);
    header.setBufferedToImage (true);
    header.toFront (false);

    footer.onAnalyzerChanged = [this] (const AnalyzerSettings& a) { display.setAnalyzer (a); hub.setAnalyzer (a); };
    footer.onMatchRequested = [this] { toggleMatch(); };
    footer.onInstancesRequested = [this] { toggleInstances(); };
    footer.onOutputRequested = [this] { toggleOutput(); };
    hub.setAnalyzer (display.getAnalyzer());
    hub.onListChanged = [] {};
    display.onMatchRequested = [this] { toggleMatch(); };
    display.onSelectionChanged = [this] { updatePanel(); };
    display.onFrame = [this] (double dt) { animate (dt); };

    // restore the selection
    juce::Array<int> sel;
    for (auto& s : juce::StringArray::fromTokens (proc.uiState.getProperty ("selection").toString(), ",", ""))
        if (s.isNotEmpty() && proc.getBand (s.getIntValue()).used) sel.add (s.getIntValue());
    const int prim = (int) proc.uiState.getProperty ("primary", -1);
    panelFade.snap (0.0f);
    display.setSelection (sel, sel.contains (prim) ? prim : (sel.isEmpty() ? -1 : sel.getLast()));
    if (panel != nullptr) { panelX.snap (targetPanelX()); panelFade.snap (1.0f); }

    relayout();
}

StEqEditor::~StEqEditor()
{
    instanceWindow.reset();
    outputPanel.reset();
    display.setCollisionReference (nullptr);
    matchPopover.reset();
    panel.reset();
    outgoing.reset();
}

void StEqEditor::updatePanel()
{
    const int b = display.getPrimary();
    const int n = display.getSelection().size();

    if (b < 0)
    {
        display.setTriggerBar (-1, -1.0f);
        if (panel != nullptr)
        {
            outgoing = std::move (panel);
            outgoing->setInterceptsMouseClicks (false, false);
            outgoingFade.snap (outgoing->getAlpha());
        }
        return;
    }

    if (panel != nullptr && panel->getBand() == b && panelCount == n)
        return;
    panelCount = n;

    const bool hadPanel = panel != nullptr;
    const bool wasExpanded = (bool) proc.uiState.getProperty ("dynExpanded", false);
    if (hadPanel)
    {
        outgoing = std::move (panel);
        outgoing->setInterceptsMouseClicks (false, false);
        outgoingFade.snap (outgoing->getAlpha());
    }

    panel = std::make_unique<BandPanel> (proc, b, n, wasExpanded);
    panel->onRemove = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<StEqEditor> (this)] { if (safe != nullptr) safe->display.removeSelected(); }); };
    panel->onPrevious = [this] { stepBand (-1); };
    panel->onNext = [this] { stepBand (1); };
    panel->onSizeChanged = [this]
    {
        if (panel != nullptr) proc.uiState.setProperty ("dynExpanded", panel->isExpanded(), nullptr);
        animate (0.0);
    };
    panel->onSplit = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Split into Left / Right");
        m.addItem (2, "Split into Mid / Side");
        st::showMenu (m, nullptr, [safe = juce::Component::SafePointer<StEqEditor> (this)] (int r)
        {
            if (safe != nullptr && r > 0) safe->display.splitSelected (r == 2);
        }, true);
    };
    panel->setBufferedToImage (true);
    getContent().addAndMakeVisible (*panel);
    header.toFront (false);

    if (! hadPanel) panelX.snap (targetPanelX());
    panelFade.snap (hadPanel ? 0.35f : 0.0f);
    panel->setAlpha (panelFade.current);
    animate (0.0);
}

void StEqEditor::stepBand (int dir)
{
    std::vector<std::pair<float, int>> order;
    for (int b = 0; b < kMaxBands; ++b)
        if (proc.getBand (b).used) order.push_back ({ proc.getBand (b).freq, b });
    if (order.empty()) return;
    std::sort (order.begin(), order.end());
    int idx = 0;
    for (int i = 0; i < (int) order.size(); ++i) if (order[(size_t) i].second == display.getPrimary()) idx = i;
    idx = (idx + dir + (int) order.size()) % (int) order.size();
    display.select (order[(size_t) idx].second);
}

void StEqEditor::toggleMatch()
{
    if (matchPopover != nullptr) { matchPopover.reset(); return; }
    matchPopover = std::make_unique<MatchPopover> (proc, display, hub);
    matchPopover->onClose = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<StEqEditor> (this)] { if (safe != nullptr) safe->matchPopover.reset(); }); };
    getContent().addAndMakeVisible (*matchPopover);
    matchPopover->setBounds ((designWidth - MatchPopover::width) / 2 - 20, 110 - MatchPopover::margin, MatchPopover::width, MatchPopover::height);
    matchPopover->toFront (true);
}

void StEqEditor::toggleInstances()
{
    if (instanceWindow != nullptr) { instanceWindow.reset(); display.grabKeyboardFocus(); return; }
    outputPanel.reset();
    matchPopover.reset();
    instanceWindow = std::make_unique<InstanceWindow> (proc, hub);
    instanceWindow->onClose = [this]
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<StEqEditor> (this)] { if (safe != nullptr && safe->instanceWindow != nullptr) safe->toggleInstances(); });
    };
    // covers the whole plugin window (header and bottom bar included) until closed
    getContent().addAndMakeVisible (*instanceWindow);
    instanceWindow->setBounds (getContent().getLocalBounds());
    instanceWindow->toFront (true);
}

void StEqEditor::toggleOutput()
{
    if (outputPanel != nullptr) { outputPanel.reset(); return; }
    outputPanel = std::make_unique<OutputPanel> (proc);
    outputPanel->onClose = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<StEqEditor> (this)] { if (safe != nullptr) safe->outputPanel.reset(); }); };
    getContent().addAndMakeVisible (*outputPanel);
    const int x = juce::jmin (designWidth - OutputPanel::panelWidth - 8, footer.getOutputButtonBounds().getRight() - OutputPanel::panelWidth);
    outputPanel->setBounds (x - OutputPanel::margin, displayHeight - 6 - OutputPanel::panelHeight - OutputPanel::margin, OutputPanel::width, OutputPanel::height);
    outputPanel->toFront (true);
}

float StEqEditor::targetPanelX() const
{
    const float w = panel != nullptr ? (float) panel->getPreferredWidth() : (float) (BandPanel::collapsedWidth + 2 * BandPanel::marginX);
    const float centre = display.getBandX (display.getPrimary());
    const float right = display.getPlotArea().getRight();
    return juce::jlimit (-(float) BandPanel::marginX + 8.0f, right - w + (float) BandPanel::marginX, centre - (float) BandPanel::anchorX);
}

void StEqEditor::animate (double dt)
{
    // other instances: spectra + collisions (only while something shows them)
    hub.update (dt, display.getSpectrum (1), instanceWindow != nullptr || matchPopover != nullptr || display.getAnalyzer().collisions);
    display.setCollisionReference (hub.getReference());

    if (panel != nullptr)
    {
        panelX.setTarget (targetPanelX());
        panelX.tick (dt, 12.0f);
        panelFade.setTarget (1.0f);
        panelFade.tick (dt, 14.0f);
        panel->setBounds (juce::roundToInt (panelX.current), panelY + juce::roundToInt ((1.0f - panelFade.current) * 8.0f),
                          panel->getPreferredWidth(), BandPanel::height);
        panel->setAlpha (panelFade.current);

        // free trigger range bar just above the island while the dynamics section is open
        if (panel->isExpanded()) display.setTriggerBar (panel->getBand(), (float) panelY - 6.0f);
        else display.setTriggerBar (-1, -1.0f);
    }

    if (outgoing != nullptr)
    {
        outgoingFade.setTarget (0.0f);
        if (! outgoingFade.tick (dt, 16.0f) || outgoingFade.current < 0.02f) outgoing.reset();
        else outgoing->setAlpha (outgoingFade.current);
    }
}

void StEqEditor::paintBackground (juce::Graphics& g, juce::Rectangle<int> area)
{
    EditorBase::paintBackground (g, area);
    // a slightly darker strip behind the header, with a faint hairline under it
    const auto& t = st::Theme::get();
    const float w = (float) area.getWidth();
    g.setColour (t.bg0.withAlpha (0.55f));
    g.fillRect (0.0f, 0.0f, w, (float) EditorBase::headerHeight);
    st::paint::fadedLine (g, { 0.0f, (float) EditorBase::headerHeight - 0.5f }, { w, (float) EditorBase::headerHeight - 0.5f },
                          juce::Colours::white.withAlpha (0.06f), 1.0f, 0.05f);
}

void StEqEditor::layout (juce::Rectangle<int>)
{
    auto all = getContent().getLocalBounds();
    display.setBounds (all.withHeight (displayHeight));
    if (instanceWindow != nullptr) instanceWindow->setBounds (all);
    footer.setBounds (all.removeFromBottom (footerHeight));
    panelX.snap (targetPanelX());
    animate (0.0);
}
