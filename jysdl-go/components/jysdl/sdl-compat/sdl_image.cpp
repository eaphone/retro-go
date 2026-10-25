/*
 * SDL_image-compat implementation using lodepng (bundled with retro-go).
 */
#include "SDL3_image/SDL_image.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* lodepng.h has no extern "C" guards, so forward-declare only what we need. */
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h, const unsigned char* in, size_t insize);

static SDL_Surface *decode_png(const Uint8 *data, size_t len)
{
    unsigned w = 0, h = 0;
    unsigned char *rgba = NULL;
    unsigned err = lodepng_decode32(&rgba, &w, &h, data, len);
    if (err != 0 || !rgba) {
        free(rgba);
        return NULL;
    }
    SDL_Surface *s = SDL_CreateSurface((int)w, (int)h, SDL_PIXELFORMAT_ARGB8888);
    Uint32 *dst = (Uint32 *)s->pixels;
    for (unsigned i = 0; i < w * h; i++) {
        Uint8 r = rgba[i * 4 + 0];
        Uint8 g = rgba[i * 4 + 1];
        Uint8 b = rgba[i * 4 + 2];
        Uint8 a = rgba[i * 4 + 3];
        dst[i] = ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
    }
    free(rgba);
    return s;
}

int IMG_isPNG(SDL_IOStream *src)
{
    if (!src) return 0;
    size_t len = 0;
    Uint8 *data = SDL_IOStreamReadAll(src, &len);
    if (!data) return 0;
    int is_png = (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G');
    free(data);
    return is_png;
}

SDL_Surface *IMG_Load(const char *file)
{
    size_t len = 0;
    void *data = SDL_LoadFile(file, &len);
    if (!data) return NULL;
    SDL_Surface *s = decode_png((const Uint8 *)data, len);
    free(data);
    return s;
}

SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file)
{
    SDL_Surface *s = IMG_Load(file);
    if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
    SDL_DestroySurface(s);
    return t;
}

SDL_Surface *IMG_Load_IO(SDL_IOStream *src, bool closeio)
{
    if (!src) return NULL;
    size_t len = 0;
    Uint8 *data = SDL_IOStreamReadAll(src, &len);
    if (closeio) SDL_CloseIO(src);
    if (!data) return NULL;
    SDL_Surface *s = decode_png(data, len);
    free(data);
    return s;
}

SDL_Surface *IMG_LoadTyped_IO(SDL_IOStream *src, bool closeio, const char *type)
{
    (void)type;
    return IMG_Load_IO(src, closeio);
}

SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc)
{
    (void)src; (void)freesrc;
    return NULL;
}
