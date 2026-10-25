/*
 * SDL_ttf-compat stub. Text is rendered as solid placeholder blocks until a
 * real font renderer (FreeType or a prerendered CJK bitmap font) is wired up.
 */
#include "SDL3_ttf/SDL_ttf.h"

#include <stdlib.h>
#include <string.h>

int TTF_Init(void) { return 0; }
void TTF_Quit(void) {}

TTF_Font *TTF_OpenFont(const char *file, int ptsize)
{
    TTF_Font *f = (TTF_Font *)calloc(1, sizeof(*f));
    f->size = ptsize > 0 ? ptsize : 16;
    f->height = f->size;
    f->name = strdup(file ? file : "");
    return f;
}

void TTF_CloseFont(TTF_Font *font)
{
    if (!font) return;
    free(font->name);
    free(font);
}

static SDL_Surface *render_glyph_placeholder(TTF_Font *font, Uint32 ch, SDL_Color fg)
{
    int w = (ch <= 128) ? ((font->size / 2) > 0 ? font->size / 2 : 1) : font->size;
    int h = font->height > 0 ? font->height : font->size;
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    Uint32 color = ((Uint32)fg.a << 24) | ((Uint32)fg.r << 16) | ((Uint32)fg.g << 8) | fg.b;
    Uint32 *p = (Uint32 *)s->pixels;
    for (int i = 0; i < w * h; i++) p[i] = color;
    return s;
}

SDL_Surface *TTF_RenderGlyph_Blended(TTF_Font *font, Uint32 ch, SDL_Color fg)
{
    if (!font) return NULL;
    return render_glyph_placeholder(font, ch, fg);
}

SDL_Surface *TTF_RenderUNICODE_Blended(TTF_Font *font, const Uint16 *text, SDL_Color fg)
{
    if (!font || !text) return NULL;
    return render_glyph_placeholder(font, text[0] ? text[0] : ' ', fg);
}

SDL_Surface *TTF_RenderUNICODE_Solid(TTF_Font *font, const Uint16 *text, SDL_Color fg)
{ return TTF_RenderUNICODE_Blended(font, text, fg); }

SDL_Surface *TTF_RenderText_Blended(TTF_Font *font, const char *text, SDL_Color fg)
{
    if (!font || !text) return NULL;
    return render_glyph_placeholder(font, (Uint32)(unsigned char)text[0], fg);
}

SDL_Surface *TTF_RenderText_Solid(TTF_Font *font, const char *text, SDL_Color fg)
{ return TTF_RenderText_Blended(font, text, fg); }

int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h)
{
    if (w) *w = font ? font->size : 16;
    if (h) *h = font ? font->height : 16;
    (void)text;
    return 0;
}
