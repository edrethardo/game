// audio_settings.h — pure, header-only helpers for the volume-options sliders.
//
// Kept SDL-free (only depends on core/types.h) so the clamp/step logic and the shipped default
// levels are unit-testable without pulling <SDL_mixer.h> into the test binary — audio.cpp can't be
// linked into tests for that reason. Shared by audio.cpp (loadSettings) and engine_menu.cpp
// (the Left/Right slider input) so the [0,1] clamp and 0.05 step are single-sourced.
#pragma once

#include "core/types.h"

namespace AudioSettings {

// Shipped default volume levels (0..1). Single source of truth for both the startup init
// (engine_init.cpp) and the options screen's "Reset to Defaults". Music sits below SFX.
inline constexpr f32 DEFAULT_MASTER = 1.0f;
inline constexpr f32 DEFAULT_SFX    = 1.0f;
inline constexpr f32 DEFAULT_MUSIC  = 0.3f;

// Options-menu Left/Right increment.
inline constexpr f32 VOLUME_STEP = 0.05f;

// Clamp a volume level into the valid [0,1] range.
inline f32 clampVol(f32 v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// Apply one Left/Right step (sign = +1 or -1) to a volume and re-clamp to [0,1].
inline f32 stepVol(f32 v, f32 sign, f32 step = VOLUME_STEP) {
    return clampVol(v + sign * step);
}

} // namespace AudioSettings
