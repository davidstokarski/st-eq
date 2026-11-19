#include "Instances.h"

namespace steq
{

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

void InstanceHub::instanceRemoved (uint64_t, st::ProcessorBase* p)
{
    for (size_t i = 0; i < others.size(); ++i)
        if (p != nullptr && others[i]->proc == p)
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

float InstanceHub::collisionFor (const st::ProcessorBase& p) const
{
    for (auto& e : others)
        if (e->proc == &p) return e->collision;
    return 0.0f;
}

//==============================================================================
EqStrip::Power::Power (StEqProcessor& p)
    : attachment (p.param (ids::bypass), [this] (float v) { on = v >= 0.5f; repaint(); }, &p.undoManager)
{
    attachment.sendInitialUpdate();
    setTooltip ("Bypass this EQ");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void EqStrip::Power::paint (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    st::icons::draw (g, st::icons::Icon::power, getLocalBounds().toFloat().reduced (3.0f), on ? t.warning : t.textDim.withAlpha (0.8f), 1.4f);
}

void EqStrip::Power::mouseDown (const juce::MouseEvent&)
{
    attachment.setValueAsCompleteGesture (on ? 0.0f : 1.0f);
}

EqStrip::EqStrip (StEqProcessor& p) : display (p, true), power (p)
{
    addAndMakeVisible (display);
    addAndMakeVisible (power);
}

void EqStrip::resized()
{
    display.setBounds (getLocalBounds());
    power.setBounds (3, 3, 20, 20);
}

} // namespace steq
