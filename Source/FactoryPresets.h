#pragma once
#include <st_shell/st_shell.h>

namespace steq
{
/** Registers the built-in presets (called from the processor constructor). */
void registerFactoryPresets (st::PresetManager&);
}

using steq::registerFactoryPresets;
