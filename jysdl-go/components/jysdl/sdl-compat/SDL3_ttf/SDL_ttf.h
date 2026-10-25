/*
 * SDL_ttf-compat: minimal stub.
 *
 * A real port needs FreeType (or a prerendered CJK bitmap font) to render the
 * game's text. For now TTF_RenderGlyph_Blended returns a solid placeholder
 * surface so the engine compiles and boots (text is drawn as blocks).
 */
#pragma once

#include "SDL3/SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TTF_Font {
    int size;
    int height;   /* pixel height of the glyph surface we synthesize */
    char *name;
} TTF_Font;

int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
void TTF_CloseFont(TTF_Font *font);

SDL_Surface *TTF_RenderGlyph_Blended(TTF_Font *font, Uint32 ch, SDL_Color fg);
SDL_Surface *TTF_RenderUNICODE_Blended(TTF_Font *font, const Uint16 *text, SDL_Color fg);
SDL_Surface *TTF_RenderUNICODE_Solid(TTF_Font *font, const Uint16 *text, SDL_Color fg);
SDL_Surface *TTF_RenderText_Blended(TTF_Font *font, const char *text, SDL_Color fg);
SDL_Surface *TTF_RenderText_Solid(TTF_Font *font, const char *text, SDL_Color fg);
int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h);

#ifdef __cplusplus
}
#endif
