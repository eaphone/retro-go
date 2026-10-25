/*
 * SDL_image-compat: PNG/WebP image loading via retro-go's lodepng.
 */
#pragma once

#include "SDL3/SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load an image file (PNG/WebP) into a 32-bit surface. */
SDL_Surface *IMG_Load(const char *file);

/* Load an image file directly into a texture. */
SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file);

/* Load from an SDL_IOStream, detecting format from data. */
SDL_Surface *IMG_Load_IO(SDL_IOStream *src, bool closeio);

/* Load from an SDL_IOStream with an explicit type hint ("png"/"webp"). */
SDL_Surface *IMG_LoadTyped_IO(SDL_IOStream *src, bool closeio, const char *type);

/* Legacy SDL2-style loader. */
SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc);

/* Return 1 if the memory block begins with a PNG signature. */
int IMG_isPNG(SDL_IOStream *src);

#ifdef __cplusplus
}
#endif
