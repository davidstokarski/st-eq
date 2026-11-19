#pragma once
#include "../Params.h"

namespace steq::sketch
{

struct FitBand
{
    Shape shape = Shape::bell;
    float freq = 1000.0f, gain = 0.0f, q = 1.0f;
    int slope = 1;
};

/** Fits up to `maxBands` bells / shelves to a target curve (points sorted by frequency, dB).
    Only the drawn frequency span is fitted. Greedy: each step adds the candidate (bell at the largest
    residual, or a shelf at either end) that reduces the error most after a local search, then all
    bands are refined together. Used by EQ Sketch (and usable for matching). */
std::vector<FitBand> fit (const std::vector<std::pair<float, float>>& target, int maxBands = 8, float toleranceDb = 0.6f);

/** dB response of a set of fitted bands at hz (analog). */
float response (const std::vector<FitBand>&, float hz);

} // namespace steq::sketch
