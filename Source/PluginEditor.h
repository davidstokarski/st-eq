#pragma once
#include "PluginProcessor.h"
#include "ui/EqDisplay.h"
#include "ui/Panels.h"
#include "ui/Instances.h"

class StEqEditor : public st::EditorBase
{
public:
    explicit StEqEditor (StEqProcessor&);
    ~StEqEditor() override;

    static constexpr int designWidth = 1040, designHeight = 620;
    static constexpr int footerHeight = 34, displayHeight = designHeight - footerHeight;
    /** Island top: its body sits just above the frequency labels. */
    static constexpr int panelY = displayHeight - 18 - steq::BandPanel::height + 6;

    steq::EqDisplay& getDisplay() noexcept { return display; }
    steq::BandPanel* getBandPanel() noexcept { return panel.get(); }
    void openDynamicsPopover() { if (panel != nullptr) panel->setExpanded (true); }
    void openMatch() { if (matchPopover == nullptr) toggleMatch(); }
    /** Opens the instance list window (onDesktop false: as a plain component, for tests / renders). */
    void openInstances (bool onDesktop = true) { if (instanceWindow == nullptr) toggleInstances (onDesktop); }
    void openOutput() { if (outputPanel == nullptr) toggleOutput(); }
    steq::InstanceHub& getInstanceHub() noexcept { return hub; }
    steq::InstanceWindow* getInstanceWindow() noexcept { return instanceWindow.get(); }

private:
    StEqProcessor& proc;
    steq::EqDisplay display;
    steq::FooterBar footer;
    steq::InstanceHub hub;
    std::unique_ptr<steq::InstanceWindow> instanceWindow;
    std::unique_ptr<steq::OutputPanel> outputPanel;
    std::unique_ptr<steq::BandPanel> panel, outgoing;
    std::unique_ptr<steq::MatchPopover> matchPopover;
    st::Animated panelX, panelFade, outgoingFade;
    int panelCount = -1;

    void layout (juce::Rectangle<int> body) override;
    void paintBackground (juce::Graphics&, juce::Rectangle<int>) override;
    void updatePanel();
    void animate (double dt);
    float targetPanelX() const;
    void stepBand (int direction);
    void toggleMatch();
    void toggleInstances (bool onDesktop = true);
    void toggleOutput();
};
