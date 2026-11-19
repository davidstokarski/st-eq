#include "FactoryPresets.h"
#include "Params.h"

namespace steq
{

namespace
{
    using Values = std::vector<std::pair<juce::String, float>>;

    /** Slope index for a dB/oct value (see slopeNames). */
    float slope (int dbPerOct)
    {
        const int i = slopeNames.indexOf (juce::String (dbPerOct) + " dB/oct");
        jassert (i >= 0);
        return (float) juce::jmax (0, i);
    }

    /** Fluent helper: bands are added to consecutive slots. */
    struct P
    {
        Values v;
        int next = 0;

        P& band (Shape shape, float freq, float gain = 0.0f, float q = 1.0f, int dbPerOct = 12, Placement place = Placement::stereo)
        {
            const int b = next++;
            set (ids::band (b, "used"), 1.0f);
            set (ids::band (b, "shape"), (float) (int) shape);
            set (ids::band (b, "freq"), freq);
            set (ids::band (b, "gain"), gain);
            set (ids::band (b, "q"), q);
            set (ids::band (b, "slope"), slope (dbPerOct));
            return set (ids::band (b, "place"), (float) (int) place);
        }

        /** Makes the last added band dynamic. */
        P& dynamic (float range, float attackMs = 10.0f, float releaseMs = 150.0f)
        {
            const int b = next - 1;
            set (ids::band (b, "dyn_range"), range);
            set (ids::band (b, "dyn_attack"), attackMs);
            return set (ids::band (b, "dyn_release"), releaseMs);
        }

        P& spectral (float density = 50.0f)
        {
            const int b = next - 1;
            set (ids::band (b, "spectral"), 1.0f);
            return set (ids::band (b, "spec_density"), density);
        }

        P& set (const juce::String& id, float value) { v.push_back ({ id, value }); return *this; }
    };
}

void registerFactoryPresets (st::PresetManager& pm)
{
    auto add = [&pm] (const char* name, const char* category, const P& p) { pm.addFactoryPreset (name, category, p.v); };


    // ------------------------------------------------------------------ Vocals
    add ("Vocal Presence", "Vocals",
         P().band (Shape::lowCut, 80.0f, 0.0f, 0.71f, 24)
            .band (Shape::bell, 300.0f, -2.5f, 1.2f)
            .band (Shape::bell, 3200.0f, 2.5f, 0.9f)
            .band (Shape::highShelf, 10000.0f, 2.0f, 0.71f));

    add ("Vocal De-Harsh", "Vocals",
         P().band (Shape::lowCut, 90.0f, 0.0f, 0.71f, 18)
            .band (Shape::bell, 3400.0f, 0.0f, 2.5f).dynamic (-6.0f, 5.0f, 120.0f)
            .band (Shape::bell, 7200.0f, 0.0f, 3.0f).dynamic (-5.0f, 2.0f, 80.0f)
            .band (Shape::highShelf, 12000.0f, 1.5f, 0.71f));

    add ("Vocal Resonance Tamer", "Vocals",
         P().band (Shape::lowCut, 80.0f, 0.0f, 0.71f, 18)
            .band (Shape::bell, 2800.0f, 0.0f, 0.45f).dynamic (-6.0f, 3.0f, 90.0f).spectral (60.0f)
            .band (Shape::highShelf, 11000.0f, 1.0f, 0.71f));

    add ("Podcast Voice", "Vocals",
         P().band (Shape::lowCut, 70.0f, 0.0f, 0.71f, 36)
            .band (Shape::bell, 180.0f, 1.5f, 0.8f)
            .band (Shape::bell, 450.0f, -3.0f, 1.4f)
            .band (Shape::bell, 2500.0f, 2.0f, 0.7f)
            .band (Shape::highCut, 16000.0f, 0.0f, 0.71f, 12));

    // ------------------------------------------------------------------- Drums
    add ("Kick Punch", "Drums",
         P().band (Shape::lowCut, 28.0f, 0.0f, 0.71f, 24)
            .band (Shape::bell, 60.0f, 3.0f, 1.4f)
            .band (Shape::bell, 350.0f, -4.0f, 1.5f)
            .band (Shape::bell, 4000.0f, 2.5f, 1.0f));

    add ("Snare Crack", "Drums",
         P().band (Shape::lowCut, 90.0f, 0.0f, 0.71f, 18)
            .band (Shape::bell, 200.0f, 2.0f, 1.0f)
            .band (Shape::bell, 900.0f, -3.0f, 2.0f)
            .band (Shape::highShelf, 6000.0f, 3.0f, 0.71f));

    add ("Overheads Air", "Drums",
         P().band (Shape::lowCut, 160.0f, 0.0f, 0.71f, 12)
            .band (Shape::bell, 400.0f, -2.0f, 0.8f)
            .band (Shape::highShelf, 9000.0f, 2.5f, 0.71f, 6));

    // -------------------------------------------------------------------- Bass
    add ("Bass Clarity", "Bass",
         P().band (Shape::lowCut, 35.0f, 0.0f, 0.71f, 24)
            .band (Shape::bell, 80.0f, 2.0f, 1.0f)
            .band (Shape::bell, 250.0f, -3.0f, 1.2f)
            .band (Shape::bell, 800.0f, 1.5f, 1.0f));

    add ("Dynamic Low End", "Bass",
         P().band (Shape::lowShelf, 110.0f, 0.0f, 0.71f).dynamic (-6.0f, 20.0f, 200.0f)
            .band (Shape::bell, 220.0f, 0.0f, 1.5f).dynamic (-4.0f, 15.0f, 150.0f));

    // --------------------------------------------------------------------- Mix
    add ("Mix Bus Smile", "Mix",
         P().band (Shape::lowShelf, 80.0f, 1.5f, 0.71f, 6)
            .band (Shape::highShelf, 12000.0f, 1.5f, 0.71f, 6));

    add ("Mid/Side Width", "Mix",
         P().band (Shape::lowCut, 150.0f, 0.0f, 0.71f, 12, Placement::side)
            .band (Shape::highShelf, 8000.0f, 3.0f, 0.71f, 12, Placement::side)
            .band (Shape::bell, 300.0f, -1.5f, 0.9f, 12, Placement::mid));

    add ("Tilt Darker", "Mix",
         P().band (Shape::flatTilt, 1000.0f, -4.0f));

    // --------------------------------------------------------------- Mastering
    add ("Mastering Air", "Mastering",
         P().band (Shape::lowCut, 20.0f, 0.0f, 0.71f, 36)
            .band (Shape::highShelf, 14000.0f, 1.5f, 0.71f, 6)
            .set (ids::mode, 2.0f));

    add ("Gentle Master Tilt", "Mastering",
         P().band (Shape::tiltShelf, 900.0f, 1.0f, 0.71f, 6)
            .set (ids::mode, 1.0f));

    // ---------------------------------------------------------------- Utility
    add ("Telephone", "Utility",
         P().band (Shape::lowCut, 400.0f, 0.0f, 0.71f, 24)
            .band (Shape::highCut, 3500.0f, 0.0f, 0.71f, 24)
            .band (Shape::bell, 1500.0f, 6.0f, 1.0f));

    add ("Hum Remover 50 Hz", "Utility",
         P().band (Shape::notch, 50.0f, 0.0f, 12.0f)
            .band (Shape::notch, 100.0f, 0.0f, 12.0f)
            .band (Shape::notch, 150.0f, 0.0f, 12.0f)
            .band (Shape::notch, 200.0f, 0.0f, 12.0f));

    add ("Hum Remover 60 Hz", "Utility",
         P().band (Shape::notch, 60.0f, 0.0f, 12.0f)
            .band (Shape::notch, 120.0f, 0.0f, 12.0f)
            .band (Shape::notch, 180.0f, 0.0f, 12.0f)
            .band (Shape::notch, 240.0f, 0.0f, 12.0f));
}

} // namespace steq
