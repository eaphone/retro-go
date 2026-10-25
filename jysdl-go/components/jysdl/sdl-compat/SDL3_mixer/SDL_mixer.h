/*
 * SDL_mixer-compat: empty placeholder.
 *
 * jysdl-bh's Audio.cpp is reworked to use retro-go's rg_audio + TinySoundFont
 * directly, so SDL_mixer is not used. This header only exists to satisfy any
 * residual include and declares the opaque types.
 */
#pragma once

#include "SDL3/SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MIX_Audio   MIX_Audio;
typedef struct MIX_Track   MIX_Track;
typedef struct MIX_Mixer   MIX_Mixer;
typedef struct MIX_Options MIX_Options;

#ifdef __cplusplus
}
#endif
