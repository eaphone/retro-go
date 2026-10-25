/*
 * SDL3-compat implementation: maps the SDL3 subset used by jysdl-bh onto
 * retro-go's rg_surface / rg_display / rg_input / rg_audio primitives.
 */
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"

#include <rg_system.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static SDL_bool rect_intersect(SDL_Rect *out, const SDL_Rect *a, const SDL_Rect *b)
{
    int x1 = a->x > b->x ? a->x : b->x;
    int y1 = a->y > b->y ? a->y : b->y;
    int x2 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y2 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    out->x = x1; out->y = y1; out->w = x2 - x1; out->h = y2 - y1;
    return (out->w > 0 && out->h > 0) ? SDL_TRUE : SDL_FALSE;
}

static inline Uint32 pack_argb(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    return ((Uint32)a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
struct SDL_Gamepad { int id; };

static SDL_Renderer *g_renderer = NULL;
static SDL_Window *g_window = NULL;
static Uint8 g_keyboard_state[256];
static Uint32 g_last_buttons = 0xFFFFFFFF;  /* force initial event flush */
static SDL_Gamepad g_gamepad_storage;
static SDL_Gamepad *g_gamepad = NULL;

/* Event queue (small ring buffer) */
#define EVENT_QUEUE_LEN 32
static SDL_Event g_event_queue[EVENT_QUEUE_LEN];
static int g_event_head = 0;
static int g_event_tail = 0;

static bool (*g_event_filter)(void *, SDL_Event *) = NULL;
static void *g_event_filter_data = NULL;

/* The physical display framebuffer (RGB565) used by SDL_RenderPresent. */
static rg_surface_t *g_display = NULL;

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
void *SDL_malloc(size_t size) { return malloc(size); }
void *SDL_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *SDL_realloc(void *mem, size_t size) { return realloc(mem, size); }
void  SDL_free(void *mem) { free(mem); }

/* ------------------------------------------------------------------ */
/* Init / quit                                                         */
/* ------------------------------------------------------------------ */
static const char *g_last_error = "";

int SDL_Init(SDL_InitFlags flags)
{
    (void)flags;
    g_last_error = "";
    return 0;
}

int SDL_InitSubSystem(SDL_InitFlags flags)
{
    (void)flags;
    return 0;
}

void SDL_Quit(void)
{
    if (g_display) { rg_surface_free(g_display); g_display = NULL; }
    g_renderer = NULL;
    g_window = NULL;
    g_gamepad = NULL;
}

const char *SDL_GetError(void) { return g_last_error; }
void SDL_ClearError(void) { g_last_error = ""; }

int SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return SDL_TRUE; }

int SDL_setenv_unsafe(const char *name, const char *value, int overwrite)
{
    (void)name; (void)value; (void)overwrite;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Properties                                                          */
/* ------------------------------------------------------------------ */
typedef enum { PROP_PTR, PROP_STR, PROP_NUM, PROP_BOOL } SDL_PropType;

typedef struct SDL_PropEntry {
    const char *name;
    SDL_PropType type;
    union { void *p; const char *s; Sint64 n; bool b; } v;
    struct SDL_PropEntry *next;
} SDL_PropEntry;

typedef struct SDL_PropGroup {
    SDL_PropertiesID id;
    SDL_PropEntry *entries;
    struct SDL_PropGroup *next;
} SDL_PropGroup;

static SDL_PropGroup *g_props = NULL;
static SDL_PropertiesID g_prop_next = 1;

static SDL_PropGroup *prop_group_find(SDL_PropertiesID id)
{
    for (SDL_PropGroup *g = g_props; g; g = g->next)
        if (g->id == id) return g;
    return NULL;
}

static SDL_PropEntry *prop_find(SDL_PropGroup *grp, const char *name)
{
    for (SDL_PropEntry *e = grp->entries; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

static SDL_PropEntry *prop_set(SDL_PropertiesID id, const char *name)
{
    SDL_PropGroup *grp = prop_group_find(id);
    if (!grp) return NULL;
    SDL_PropEntry *e = prop_find(grp, name);
    if (!e) {
        e = (SDL_PropEntry *)calloc(1, sizeof(*e));
        e->name = strdup(name);
        e->next = grp->entries;
        grp->entries = e;
    }
    return e;
}

SDL_PropertiesID SDL_CreateProperties(void)
{
    SDL_PropGroup *grp = (SDL_PropGroup *)calloc(1, sizeof(*grp));
    grp->id = g_prop_next++;
    grp->next = g_props;
    g_props = grp;
    return grp->id;
}

bool SDL_DestroyProperties(SDL_PropertiesID props)
{
    SDL_PropGroup **pp = &g_props;
    while (*pp) {
        if ((*pp)->id == props) {
            SDL_PropGroup *g = *pp;
            *pp = g->next;
            SDL_PropEntry *e = g->entries;
            while (e) {
                SDL_PropEntry *n = e->next;
                free((void *)e->name);
                free(e);
                e = n;
            }
            free(g);
            return SDL_TRUE;
        }
        pp = &(*pp)->next;
    }
    return SDL_FALSE;
}

bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value)
{ SDL_PropEntry *e = prop_set(props, name); if (!e) return SDL_FALSE; e->type = PROP_PTR; e->v.p = value; return SDL_TRUE; }
bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value)
{ SDL_PropEntry *e = prop_set(props, name); if (!e) return SDL_FALSE; e->type = PROP_STR; e->v.s = value; return SDL_TRUE; }
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{ SDL_PropEntry *e = prop_set(props, name); if (!e) return SDL_FALSE; e->type = PROP_NUM; e->v.n = value; return SDL_TRUE; }
bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value)
{ SDL_PropEntry *e = prop_set(props, name); if (!e) return SDL_FALSE; e->type = PROP_NUM; e->v.n = (Sint64)(int)(value * 1000.0f); return SDL_TRUE; }
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{ SDL_PropEntry *e = prop_set(props, name); if (!e) return SDL_FALSE; e->type = PROP_BOOL; e->v.b = value; return SDL_TRUE; }

void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value)
{ SDL_PropGroup *g = prop_group_find(props); SDL_PropEntry *e = g ? prop_find(g, name) : NULL; return (e && e->type == PROP_PTR) ? e->v.p : default_value; }
const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value)
{ SDL_PropGroup *g = prop_group_find(props); SDL_PropEntry *e = g ? prop_find(g, name) : NULL; return (e && e->type == PROP_STR) ? e->v.s : default_value; }
Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value)
{ SDL_PropGroup *g = prop_group_find(props); SDL_PropEntry *e = g ? prop_find(g, name) : NULL; return (e && e->type == PROP_NUM) ? e->v.n : default_value; }
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{ SDL_PropGroup *g = prop_group_find(props); SDL_PropEntry *e = g ? prop_find(g, name) : NULL; return (e && e->type == PROP_BOOL) ? e->v.b : default_value; }

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props)
{
    Sint64 w = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 320);
    Sint64 h = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 240);
    if (g_window) { SDL_DestroyWindow(g_window); }
    g_window = (SDL_Window *)calloc(1, sizeof(*g_window));
    g_window->w = (int)w;
    g_window->h = (int)h;
    return g_window;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    if (window && window == g_window) { free(window); g_window = NULL; }
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
    if (window) window->fullscreen = fullscreen;
    return SDL_TRUE;
}

bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    if (window) { if (w) *w = window->w; if (h) *h = window->h; return SDL_TRUE; }
    return SDL_FALSE;
}

void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon)
{
    (void)window;
    if (icon) SDL_DestroySurface(icon);
}

const char *SDL_VideoDriverName(char *namebuf, int maxlen)
{
    if (namebuf && maxlen > 0) snprintf(namebuf, maxlen, "retro-go");
    return namebuf;
}

/* ------------------------------------------------------------------ */
/* Renderer                                                            */
/* ------------------------------------------------------------------ */
static SDL_Texture *renderer_target(SDL_Renderer *r)
{
    return r->target ? r->target : r->backbuffer;
}

SDL_Renderer *SDL_CreateRendererWithProperties(SDL_PropertiesID props)
{
    SDL_Window *win = (SDL_Window *)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, NULL);
    if (!win) win = g_window;
    if (g_renderer) SDL_DestroyRenderer(g_renderer);

    g_renderer = (SDL_Renderer *)calloc(1, sizeof(*g_renderer));
    g_renderer->window = win;
    g_renderer->draw_r = 0; g_renderer->draw_g = 0; g_renderer->draw_b = 0; g_renderer->draw_a = 255;
    g_renderer->draw_blend = SDL_BLENDMODE_NONE;
    g_renderer->has_clip = false;
    g_renderer->backbuffer = (SDL_Texture *)calloc(1, sizeof(SDL_Texture));
    g_renderer->backbuffer->format = SDL_PIXELFORMAT_ARGB8888;
    g_renderer->backbuffer->access = SDL_TEXTUREACCESS_TARGET;
    g_renderer->backbuffer->w = win ? win->w : 320;
    g_renderer->backbuffer->h = win ? win->h : 240;
    g_renderer->backbuffer->pixels = (Uint32 *)calloc((size_t)g_renderer->backbuffer->w * g_renderer->backbuffer->h, 4);
    g_renderer->backbuffer->blend_mode = SDL_BLENDMODE_NONE;
    g_renderer->backbuffer->alpha_mod = 255;
    g_renderer->backbuffer->r_mod = g_renderer->backbuffer->g_mod = g_renderer->backbuffer->b_mod = 255;
    g_renderer->backbuffer->free_pixels = true;
    return g_renderer;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
    if (!renderer) return;
    if (renderer->backbuffer) {
        if (renderer->backbuffer->free_pixels) free(renderer->backbuffer->pixels);
        free(renderer->backbuffer);
    }
    if (renderer == g_renderer) g_renderer = NULL;
    free(renderer);
}

bool SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    if (!renderer) return SDL_FALSE;
    renderer->target = texture;
    return SDL_TRUE;
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (!renderer) return SDL_FALSE;
    renderer->draw_r = r; renderer->draw_g = g; renderer->draw_b = b; renderer->draw_a = a;
    return SDL_TRUE;
}

bool SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode mode)
{
    if (!renderer) return SDL_FALSE;
    renderer->draw_blend = mode;
    return SDL_TRUE;
}

bool SDL_SetRenderClipRect(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    if (!renderer) return SDL_FALSE;
    if (rect) { renderer->clip_rect = *rect; renderer->has_clip = true; }
    else renderer->has_clip = false;
    return SDL_TRUE;
}

bool SDL_GetRenderClipRect(SDL_Renderer *renderer, SDL_Rect *rect)
{
    if (!renderer || !rect) return SDL_FALSE;
    if (renderer->has_clip) *rect = renderer->clip_rect;
    else { rect->x = 0; rect->y = 0; rect->w = 0; rect->h = 0; }
    return SDL_TRUE;
}

/* ------------------------------------------------------------------ */
/* Texture                                                             */
/* ------------------------------------------------------------------ */
SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h)
{
    (void)renderer;
    SDL_Texture *t = (SDL_Texture *)calloc(1, sizeof(*t));
    t->format = format;
    t->access = access;
    t->w = w; t->h = h;
    t->pixels = (Uint32 *)calloc((size_t)w * h, 4);
    t->blend_mode = SDL_BLENDMODE_NONE;
    t->alpha_mod = 255;
    t->r_mod = t->g_mod = t->b_mod = 255;
    t->scale_mode = SDL_SCALEMODE_NEAREST;
    t->free_pixels = true;
    return t;
}

SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface)
{
    if (!surface) return NULL;
    SDL_Texture *t = SDL_CreateTexture(renderer, surface->format ? surface->format->format : SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, surface->w, surface->h);
    Uint32 *src = (Uint32 *)surface->pixels;
    Uint32 *dst = t->pixels;
    for (int i = 0; i < surface->w * surface->h; i++) {
        Uint32 px = src[i];
        /* Apply colorkey as transparency */
        if (surface->has_color_key && (px & 0x00FFFFFF) == (surface->color_key & 0x00FFFFFF))
            px = 0x00000000;
        dst[i] = px;
    }
    return t;
}

void SDL_DestroyTexture(SDL_Texture *texture)
{
    if (!texture) return;
    if (texture->free_pixels) free(texture->pixels);
    free(texture);
}

bool SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode mode) { if (texture) texture->blend_mode = mode; return SDL_TRUE; }
bool SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha) { if (texture) texture->alpha_mod = alpha; return SDL_TRUE; }
bool SDL_SetTextureColorMod(SDL_Texture *texture, Uint8 r, Uint8 g, Uint8 b) { if (texture) { texture->r_mod = r; texture->g_mod = g; texture->b_mod = b; } return SDL_TRUE; }
bool SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode mode) { if (texture) texture->scale_mode = mode; return SDL_TRUE; }
bool SDL_GetTextureSize(SDL_Texture *texture, float *w, float *h)
{
    if (!texture) return SDL_FALSE;
    if (w) *w = (float)texture->w;
    if (h) *h = (float)texture->h;
    return SDL_TRUE;
}

/* ------------------------------------------------------------------ */
/* Surface                                                             */
/* ------------------------------------------------------------------ */
static SDL_PixelFormatSDL2 g_format_argb = {
    SDL_PIXELFORMAT_ARGB8888, NULL, 32, 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000
};

SDL_PixelFormat SDL_GetPixelFormatForMasks(int bpp, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)bpp; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return SDL_PIXELFORMAT_ARGB8888;
}

SDL_Surface *SDL_CreateSurface(int w, int h, SDL_PixelFormat format)
{
    (void)format;
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(*s));
    s->w = w; s->h = h;
    s->pitch = w * 4;
    s->pixels = calloc((size_t)w * h, 4);
    s->format = &g_format_argb;
    s->clip_rect.x = 0; s->clip_rect.y = 0; s->clip_rect.w = w; s->clip_rect.h = h;
    s->has_color_key = false;
    s->alpha_mod = 255;
    s->free_pixels = true;
    return s;
}

SDL_Surface *SDL_CreateSurfaceFrom(int w, int h, SDL_PixelFormat format, void *pixels, int pitch)
{
    (void)format; (void)pitch;
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(*s));
    s->w = w; s->h = h;
    s->pitch = w * 4;
    s->pixels = pixels;
    s->format = &g_format_argb;
    s->clip_rect.x = 0; s->clip_rect.y = 0; s->clip_rect.w = w; s->clip_rect.h = h;
    s->has_color_key = false;
    s->alpha_mod = 255;
    s->free_pixels = false;
    return s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags; (void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
}

void SDL_DestroySurface(SDL_Surface *surface)
{
    if (!surface) return;
    if (surface->free_pixels) free(surface->pixels);
    free(surface);
}

SDL_Surface *SDL_ConvertSurface(SDL_Surface *surface, SDL_PixelFormatSDL2 *format)
{
    (void)format;
    if (!surface) return NULL;
    /* Everything is 32-bit ARGB in our shim, so a convert is a copy. */
    SDL_Surface *s = SDL_CreateSurface(surface->w, surface->h, SDL_PIXELFORMAT_ARGB8888);
    memcpy(s->pixels, surface->pixels, (size_t)surface->w * surface->h * 4);
    s->has_color_key = surface->has_color_key;
    s->color_key = surface->color_key;
    s->alpha_mod = surface->alpha_mod;
    return s;
}

Uint32 SDL_MapSurfaceRGBA(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    (void)surface;
    return pack_argb(r, g, b, a);
}

bool SDL_FillSurfaceRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color)
{
    if (!dst || !dst->pixels) return SDL_FALSE;
    SDL_Rect r;
    if (rect) r = *rect; else { r.x = 0; r.y = 0; r.w = dst->w; r.h = dst->h; }
    SDL_Rect clip = { 0, 0, dst->w, dst->h };
    SDL_Rect inter;
    if (!rect_intersect(&inter, &r, &clip)) return SDL_TRUE;
    Uint32 *p = (Uint32 *)dst->pixels;
    for (int y = inter.y; y < inter.y + inter.h; y++)
        for (int x = inter.x; x < inter.x + inter.w; x++)
            p[y * dst->w + x] = color;
    return SDL_TRUE;
}

bool SDL_SetSurfaceColorKey(SDL_Surface *surface, bool enabled, Uint32 key)
{
    if (!surface) return SDL_FALSE;
    surface->has_color_key = enabled ? true : false;
    surface->color_key = key;
    return SDL_TRUE;
}

bool SDL_SetSurfaceAlphaMod(SDL_Surface *surface, Uint8 alpha) { if (surface) surface->alpha_mod = alpha; return SDL_TRUE; }

bool SDL_SetSurfaceClipRect(SDL_Surface *surface, const SDL_Rect *rect)
{
    if (!surface) return SDL_FALSE;
    if (rect) surface->clip_rect = *rect;
    else { surface->clip_rect.x = 0; surface->clip_rect.y = 0; surface->clip_rect.w = surface->w; surface->clip_rect.h = surface->h; }
    return SDL_TRUE;
}

bool SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors)
{
    if (!palette || !colors) return SDL_FALSE;
    if (firstcolor + ncolors > palette->ncolors) ncolors = palette->ncolors - firstcolor;
    if (ncolors <= 0) return SDL_FALSE;
    if (!palette->colors) palette->colors = (SDL_Color *)calloc(palette->ncolors, sizeof(SDL_Color));
    memcpy(palette->colors + firstcolor, colors, (size_t)ncolors * sizeof(SDL_Color));
    return SDL_TRUE;
}

bool SDL_LockSurface(SDL_Surface *surface) { (void)surface; return SDL_TRUE; }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }

bool SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect)
{
    if (!src || !dst || !src->pixels || !dst->pixels) return SDL_FALSE;
    SDL_Rect s = srcrect ? *srcrect : (SDL_Rect){0, 0, src->w, src->h};
    SDL_Rect d = dstrect ? *dstrect : (SDL_Rect){0, 0, s.w, s.h};
    SDL_Rect clip = { 0, 0, dst->w, dst->h };
    SDL_Rect di;
    if (!rect_intersect(&di, &d, &clip)) return SDL_TRUE;
    Uint32 *sp = (Uint32 *)src->pixels;
    Uint32 *dp = (Uint32 *)dst->pixels;
    int sa = src->alpha_mod;
    for (int y = 0; y < di.h; y++) {
        int sy = s.y + (di.y + y - d.y);
        for (int x = 0; x < di.w; x++) {
            int sx = s.x + (di.x + x - d.x);
            Uint32 px = sp[sy * src->w + sx];
            if (src->has_color_key && (px & 0x00FFFFFF) == (src->color_key & 0x00FFFFFF))
                continue;
            Uint8 a = (Uint8)(((px >> 24) & 0xFF) * sa / 255);
            Uint8 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            if (a == 0) continue;
            Uint32 dpix = dp[(di.y + y) * dst->w + (di.x + x)];
            Uint8 dr = (dpix >> 16) & 0xFF, dg = (dpix >> 8) & 0xFF, db = dpix & 0xFF;
            int na = 255 - a;
            Uint8 orr = (Uint8)((r * a + dr * na) / 255);
            Uint8 og = (Uint8)((g * a + dg * na) / 255);
            Uint8 ob = (Uint8)((b * a + db * na) / 255);
            dp[(di.y + y) * dst->w + (di.x + x)] = pack_argb(orr, og, ob, 255);
        }
    }
    return SDL_TRUE;
}

/* ------------------------------------------------------------------ */
/* Rendering primitives (into ARGB8888 buffers)                        */
/* ------------------------------------------------------------------ */
static inline void blend_pixel(Uint32 *dst, Uint32 spx, SDL_BlendMode mode, Uint8 amod, Uint8 rmod, Uint8 gmod, Uint8 bmod)
{
    /* Skip the /255 division when the mod is identity (the common case). */
    Uint8 sa = (amod == 255) ? (Uint8)(spx >> 24) : (Uint8)(((spx >> 24) & 0xFF) * amod / 255);
    Uint8 sr = (rmod == 255) ? (Uint8)(spx >> 16) : (Uint8)(((spx >> 16) & 0xFF) * rmod / 255);
    Uint8 sg = (gmod == 255) ? (Uint8)(spx >> 8)  : (Uint8)(((spx >> 8) & 0xFF) * gmod / 255);
    Uint8 sb = (bmod == 255) ? (Uint8)spx         : (Uint8)((spx & 0xFF) * bmod / 255);

    if (mode == SDL_BLENDMODE_NONE || sa == 255) {
        /* No blending needed: opaque source simply overwrites the destination. */
        *dst = pack_argb(sr, sg, sb, sa);
        return;
    }
    if (sa == 0) return;  /* fully transparent: destination unchanged */

    Uint32 dpix = *dst;
    Uint8 da = (dpix >> 24) & 0xFF, dr = (dpix >> 16) & 0xFF, dg = (dpix >> 8) & 0xFF, db = dpix & 0xFF;

    if (mode == SDL_BLENDMODE_BLEND) {
        int na = 255 - sa;
        /* Fast alpha blend: /255 approximated with /256 (>>8). */
        Uint8 orr = (Uint8)(dr + (((sr - dr) * sa) >> 8));
        Uint8 og = (Uint8)(dg + (((sg - dg) * sa) >> 8));
        Uint8 ob = (Uint8)(db + (((sb - db) * sa) >> 8));
        Uint8 oa = (Uint8)(sa + ((da * na) >> 8));
        *dst = pack_argb(orr, og, ob, oa);
    } else { /* SDL_BLENDMODE_ADD */
        Uint8 orr = (sr + dr > 255) ? 255 : sr + dr;
        Uint8 og = (sg + dg > 255) ? 255 : sg + dg;
        Uint8 ob = (sb + db > 255) ? 255 : sb + db;
        *dst = pack_argb(orr, og, ob, 255);
    }
}

static bool renderer_clip_rect(SDL_Renderer *r, SDL_Texture *target, SDL_Rect *out)
{
    SDL_Rect full = { 0, 0, target->w, target->h };
    if (r->has_clip) {
        if (!rect_intersect(out, &r->clip_rect, &full)) return false;
    } else {
        *out = full;
    }
    return true;
}

bool SDL_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect)
{
    if (!renderer || !texture || !texture->pixels) return SDL_FALSE;
    SDL_Texture *target = renderer_target(renderer);
    if (!target || !target->pixels) return SDL_FALSE;

    SDL_FRect s = srcrect ? *srcrect : (SDL_FRect){0, 0, (float)texture->w, (float)texture->h};
    SDL_FRect d = dstrect ? *dstrect : (SDL_FRect){0, 0, (float)texture->w, (float)texture->h};

    SDL_Rect clip;
    if (!renderer_clip_rect(renderer, target, &clip)) return SDL_TRUE;

    int dx0 = (int)d.x, dy0 = (int)d.y;
    int dx1 = (int)(d.x + d.w), dy1 = (int)(d.y + d.h);
    if (dx0 < clip.x) dx0 = clip.x;
    if (dy0 < clip.y) dy0 = clip.y;
    if (dx1 > clip.x + clip.w) dx1 = clip.x + clip.w;
    if (dy1 > clip.y + clip.h) dy1 = clip.y + clip.h;

    int dw = dx1 - dx0, dh = dy1 - dy0;
    if (dw <= 0 || dh <= 0) return SDL_TRUE;

    /* Fast path: full-surface, 1:1, opaque, no color mods, no clip -> plain copy. */
    if (!srcrect && !dstrect && texture->w == target->w && texture->h == target->h &&
        texture->blend_mode == SDL_BLENDMODE_NONE && texture->alpha_mod == 255 &&
        texture->r_mod == 255 && texture->g_mod == 255 && texture->b_mod == 255 &&
        !renderer->has_clip) {
        memcpy(target->pixels, texture->pixels, (size_t)texture->w * texture->h * 4);
        return SDL_TRUE;
    }

    float sx0 = s.x, sy0 = s.y;
    float scale_x = s.w / d.w;
    float scale_y = s.h / d.h;

    Uint32 *src = texture->pixels;
    Uint32 *dst = target->pixels;

    /* Fast path: 1:1 blit (no scaling) — avoids per-pixel float math. */
    if (s.w == d.w && s.h == d.h) {
        int sx_off = (int)(s.x - d.x);
        int sy_off = (int)(s.y - d.y);
        for (int y = 0; y < dh; y++) {
            int sy = y + dy0 + sy_off;
            if (sy < 0) sy = 0;
            if (sy >= texture->h) sy = texture->h - 1;
            Uint32 *src_row = &src[sy * texture->w];
            Uint32 *dst_row = &dst[(dy0 + y) * target->w + dx0];
            for (int x = 0; x < dw; x++) {
                int sxx = x + dx0 + sx_off;
                if (sxx < 0) sxx = 0;
                if (sxx >= texture->w) sxx = texture->w - 1;
                blend_pixel(&dst_row[x], src_row[sxx], texture->blend_mode,
                            texture->alpha_mod, texture->r_mod, texture->g_mod, texture->b_mod);
            }
        }
        return SDL_TRUE;
    }

    for (int y = 0; y < dh; y++) {
        int sy = (int)(sy0 + (dy0 + y - d.y) * scale_y);
        if (sy < 0) sy = 0;
        if (sy >= texture->h) sy = texture->h - 1;
        for (int x = 0; x < dw; x++) {
            int sxx = (int)(sx0 + (dx0 + x - d.x) * scale_x);
            if (sxx < 0) sxx = 0;
            if (sxx >= texture->w) sxx = texture->w - 1;
            Uint32 spx = src[sy * texture->w + sxx];
            blend_pixel(&dst[(dy0 + y) * target->w + (dx0 + x)], spx,
                        texture->blend_mode, texture->alpha_mod, texture->r_mod, texture->g_mod, texture->b_mod);
        }
    }
    return SDL_TRUE;
}

bool SDL_RenderTextureRotated(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect, double angle, const SDL_FPoint *center, SDL_FlipMode flip)
{
    (void)center;
    /* Fast paths for the common cases used by the engine. */
    if (angle == 0.0 || angle == 360.0)
        return SDL_RenderTexture(renderer, texture, srcrect, dstrect);

    /* For 90/180/270 degree rotations (screen rotate), do a per-pixel rotate. */
    if (!renderer || !texture || !texture->pixels) return SDL_FALSE;
    SDL_Texture *target = renderer_target(renderer);
    if (!target || !target->pixels) return SDL_FALSE;
    SDL_FRect d = dstrect ? *dstrect : (SDL_FRect){0, 0, (float)texture->w, (float)texture->h};
    SDL_Rect clip;
    if (!renderer_clip_rect(renderer, target, &clip)) return SDL_TRUE;

    Uint32 *src = texture->pixels;
    Uint32 *dst = target->pixels;
    int dw = (int)d.w, dh = (int)d.h;
    int sw = texture->w, sh = texture->h;
    int cx = (int)d.x, cy = (int)d.y;

    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int tx = x, ty = y;
            if (flip & SDL_FLIP_HORIZONTAL) tx = dw - 1 - x;
            if (flip & SDL_FLIP_VERTICAL) ty = dh - 1 - y;
            int sx = tx, sy = ty;
            if (angle == 90.0)      { sx = ty; sy = dw - 1 - tx; }
            else if (angle == 180.0){ sx = dw - 1 - tx; sy = dh - 1 - ty; }
            else if (angle == 270.0){ sx = dh - 1 - ty; sy = tx; }
            else {
                /* Arbitrary angle: nearest-neighbor via inverse rotation. */
                double rad = angle * 3.14159265358979323846 / 180.0;
                double ix = (double)(tx - dw / 2) * cos(rad) - (double)(ty - dh / 2) * sin(rad) + (double)(sw / 2);
                double iy = (double)(tx - dw / 2) * sin(rad) + (double)(ty - dh / 2) * cos(rad) + (double)(sh / 2);
                sx = (int)ix; sy = (int)iy;
            }
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            if (sy < 0) sy = 0;
            if (sy >= sh) sy = sh - 1;
            int dx = cx + x, dy = cy + y;
            if (dx < clip.x || dy < clip.y || dx >= clip.x + clip.w || dy >= clip.y + clip.h) continue;
            Uint32 spx = src[sy * sw + sx];
            blend_pixel(&dst[dy * target->w + dx], spx,
                        texture->blend_mode, texture->alpha_mod, texture->r_mod, texture->g_mod, texture->b_mod);
        }
    }
    return SDL_TRUE;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect)
{
    if (!renderer) return SDL_FALSE;
    SDL_Texture *target = renderer_target(renderer);
    if (!target || !target->pixels) return SDL_FALSE;
    SDL_Rect clip;
    if (!renderer_clip_rect(renderer, target, &clip)) return SDL_TRUE;

    SDL_FRect f = rect ? *rect : (SDL_FRect){0, 0, (float)target->w, (float)target->h};
    int x0 = (int)f.x, y0 = (int)f.y, x1 = (int)(f.x + f.w), y1 = (int)(f.y + f.h);
    if (x0 < clip.x) x0 = clip.x;
    if (y0 < clip.y) y0 = clip.y;
    if (x1 > clip.x + clip.w) x1 = clip.x + clip.w;
    if (y1 > clip.y + clip.h) y1 = clip.y + clip.h;

    Uint32 spx = pack_argb(renderer->draw_r, renderer->draw_g, renderer->draw_b, renderer->draw_a);
    Uint32 *dst = target->pixels;

    /* Fast path: fully opaque fill (none or blend) -> plain overwrite. */
    if (renderer->draw_a == 255 && (renderer->draw_blend == SDL_BLENDMODE_NONE || renderer->draw_blend == SDL_BLENDMODE_BLEND)) {
        for (int y = y0; y < y1; y++) {
            Uint32 *row = &dst[y * target->w + x0];
            for (int x = 0; x < x1 - x0; x++) row[x] = spx;
        }
        return SDL_TRUE;
    }

    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            blend_pixel(&dst[y * target->w + x], spx, renderer->draw_blend, 255, 255, 255, 255);
    return SDL_TRUE;
}

bool SDL_RenderLine(SDL_Renderer *renderer, float x1, float y1, float x2, float y2)
{
    if (!renderer) return SDL_FALSE;
    SDL_Texture *target = renderer_target(renderer);
    if (!target || !target->pixels) return SDL_FALSE;
    SDL_Rect clip;
    if (!renderer_clip_rect(renderer, target, &clip)) return SDL_TRUE;
    Uint32 spx = pack_argb(renderer->draw_r, renderer->draw_g, renderer->draw_b, renderer->draw_a);
    Uint32 *dst = target->pixels;

    int xa = (int)x1, ya = (int)y1, xb = (int)x2, yb = (int)y2;
    int dx = xb - xa, dy = yb - ya;
    int steps = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int x = xa + (int)((float)dx * i / steps);
        int y = ya + (int)((float)dy * i / steps);
        if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) continue;
        blend_pixel(&dst[y * target->w + x], spx, renderer->draw_blend, 255, 255, 255, 255);
    }
    return SDL_TRUE;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
    if (!renderer || !renderer->backbuffer) return SDL_FALSE;
    SDL_Texture *bb = renderer->backbuffer;
    int sw = bb->w, sh = bb->h;
    int dw = RG_SCREEN_WIDTH, dh = RG_SCREEN_HEIGHT;

    if (!g_display)
        g_display = rg_surface_create(dw, dh, RG_PIXEL_565_LE, MEM_FAST);

    if (!g_display || !g_display->data) return SDL_FALSE;

    uint16_t *dst = (uint16_t *)g_display->data;
    Uint32 *src = bb->pixels;

    if (sw == dw && sh == dh) {
        /* 1:1 backbuffer -> display (the common case): no per-pixel scaling math. */
        for (int i = 0; i < dw * dh; i++) {
            Uint32 px = src[i];
            Uint8 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    } else {
        for (int y = 0; y < dh; y++) {
            int sy = (int)((float)y * sh / dh);
            if (sy >= sh) sy = sh - 1;
            for (int x = 0; x < dw; x++) {
                int sx = (int)((float)x * sw / dw);
                if (sx >= sw) sx = sw - 1;
                Uint32 px = src[sy * sw + sx];
                Uint8 r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                dst[y * dw + x] = c;
            }
        }
    }

    rg_display_submit(g_display, 0);
    return SDL_TRUE;
}

/* ------------------------------------------------------------------ */
/* Input / events                                                      */
/* ------------------------------------------------------------------ */
bool SDL_PushEvent(SDL_Event *event)
{
    if (!event) return SDL_FALSE;
    int next = (g_event_head + 1) % EVENT_QUEUE_LEN;
    if (next == g_event_tail) return SDL_FALSE; /* full */
    g_event_queue[g_event_head] = *event;
    g_event_head = next;
    return SDL_TRUE;
}

void SDL_SetEventFilter(bool (*filter)(void *userdata, SDL_Event *event), void *userdata)
{
    g_event_filter = filter;
    g_event_filter_data = userdata;
}

void SDL_SetGamepadEventsEnabled(bool enabled) { (void)enabled; }

/* Map retro-go RG_KEY_* -> SDL_GAMEPAD_BUTTON_* */
static const struct { Uint32 rg; Uint8 sdl; } g_button_map[] = {
    { RG_KEY_UP,     SDL_GAMEPAD_BUTTON_DPAD_UP },
    { RG_KEY_DOWN,   SDL_GAMEPAD_BUTTON_DPAD_DOWN },
    { RG_KEY_LEFT,   SDL_GAMEPAD_BUTTON_DPAD_LEFT },
    { RG_KEY_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    { RG_KEY_A,      SDL_GAMEPAD_BUTTON_SOUTH },
    { RG_KEY_B,      SDL_GAMEPAD_BUTTON_EAST },
    { RG_KEY_X,      SDL_GAMEPAD_BUTTON_WEST },
    { RG_KEY_Y,      SDL_GAMEPAD_BUTTON_NORTH },
    { RG_KEY_START,  SDL_GAMEPAD_BUTTON_START },
    { RG_KEY_SELECT, SDL_GAMEPAD_BUTTON_BACK },
    { RG_KEY_L,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
    { RG_KEY_R,      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
};

/* Key repeat (SDL_EnableKeyRepeat) for held gamepad buttons. */
static int g_key_repeat_delay = 250;
static int g_key_repeat_interval = 30;
static Uint64 g_key_down_time[sizeof(g_button_map) / sizeof(g_button_map[0])];
static Uint64 g_key_last_repeat[sizeof(g_button_map) / sizeof(g_button_map[0])];

void SDL_EnableKeyRepeat(int delay, int interval)
{
    /* The game requests 30/30ms (KeyRepeatDelay/KeyRePeatInterval), which is
     * far too fast for menu navigation on this hardware. Clamp to comfortable
     * minimums. delay==0 keeps repeat off (the game uses interval=0 for that). */
    if (delay > 0)
    {
        if (delay < 250) delay = 250;
        /* The game re-enables with interval=0 due to a config typo
         * (KeyRepeatInterval vs KeyRePeatInterval); use a sane default. */
        if (interval <= 0) interval = 100;
        if (interval < 100) interval = 100;
    }
    g_key_repeat_delay = delay;
    g_key_repeat_interval = interval;
}

static void synthesize_gamepad_events(void)
{
    Uint32 buttons = rg_input_read_gamepad();
    Uint32 changed = g_last_buttons ^ buttons;
    Uint64 now = SDL_GetTicks();

    /* Handle retro-go system menu on MENU/OPTION press. */
    if (changed & RG_KEY_MENU && (buttons & RG_KEY_MENU)) {
        rg_gui_game_menu();
        g_last_buttons = buttons;
        return;
    }
    if (changed & RG_KEY_OPTION && (buttons & RG_KEY_OPTION)) {
        rg_gui_options_menu();
        g_last_buttons = buttons;
        return;
    }

    const size_t nbuttons = sizeof(g_button_map) / sizeof(g_button_map[0]);
    for (size_t i = 0; i < nbuttons; i++) {
        Uint32 btn = g_button_map[i].rg;

        if (changed & btn) {
            SDL_Event e;
            memset(&e, 0, sizeof(e));
            if (buttons & btn) {
                e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                e.gbutton.down = true;
                g_key_down_time[i] = now;
                g_key_last_repeat[i] = now;
            } else {
                e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
                e.gbutton.down = false;
            }
            e.gbutton.which = 0;
            e.gbutton.button = g_button_map[i].sdl;
            SDL_PushEvent(&e);
        }
        else if ((buttons & btn) && g_key_repeat_interval > 0 &&
                 now - g_key_down_time[i] >= (Uint64)g_key_repeat_delay &&
                 now - g_key_last_repeat[i] >= (Uint64)g_key_repeat_interval)
        {
            SDL_Event e;
            memset(&e, 0, sizeof(e));
            e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            e.gbutton.down = true;
            e.gbutton.which = 0;
            e.gbutton.button = g_button_map[i].sdl;
            SDL_PushEvent(&e);
            g_key_last_repeat[i] = now;
        }
    }
    g_last_buttons = buttons;
}

bool SDL_PollEvent(SDL_Event *event)
{
    synthesize_gamepad_events();

    if (g_event_head == g_event_tail)
        return SDL_FALSE;  /* no events */

    SDL_Event e = g_event_queue[g_event_tail];
    g_event_tail = (g_event_tail + 1) % EVENT_QUEUE_LEN;

    if (g_event_filter && !g_event_filter(g_event_filter_data, &e))
        return SDL_PollEvent(event);  /* filtered; try next */

    *event = e;
    return SDL_TRUE;
}

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    if (numkeys) *numkeys = (int)sizeof(g_keyboard_state);
    return g_keyboard_state;
}

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate)
{
    if (modstate) *modstate = 0;
    switch (key) {
    case SDLK_UP: return SDL_SCANCODE_UP;
    case SDLK_DOWN: return SDL_SCANCODE_DOWN;
    case SDLK_LEFT: return SDL_SCANCODE_LEFT;
    case SDLK_RIGHT: return SDL_SCANCODE_RIGHT;
    case SDLK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case SDLK_RETURN: return SDL_SCANCODE_RETURN;
    case SDLK_SPACE: return SDL_SCANCODE_SPACE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

SDL_JoystickID *SDL_GetJoysticks(int *count)
{
    static SDL_JoystickID ids[1];
    ids[0] = 0;
    if (count) *count = 1;
    return ids;
}

bool SDL_IsGamepad(SDL_JoystickID instance_id) { return instance_id == 0; }

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id)
{
    if (instance_id != 0) return NULL;
    g_gamepad = &g_gamepad_storage;
    return g_gamepad;
}

void SDL_CloseGamepad(SDL_Gamepad *gamepad)
{
    if (gamepad == &g_gamepad_storage) g_gamepad = NULL;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
Uint64 SDL_GetTicks(void)
{
    return (Uint64)(rg_system_timer() / 1000);
}

void SDL_Delay(Uint32 ms)
{
    rg_task_delay(ms);
}

/* ------------------------------------------------------------------ */
/* IO                                                                  */
/* ------------------------------------------------------------------ */
struct SDL_IOStream {
    FILE *fp;
    Uint8 *mem;
    size_t size;
    size_t pos;
};

SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode)
{
    FILE *fp = fopen(file, mode);
    if (!fp) return NULL;
    SDL_IOStream *io = (SDL_IOStream *)calloc(1, sizeof(*io));
    io->fp = fp;
    return io;
}

SDL_IOStream *SDL_IOFromMem(void *mem, size_t size)
{
    SDL_IOStream *io = (SDL_IOStream *)calloc(1, sizeof(*io));
    io->mem = (Uint8 *)mem;
    io->size = size;
    io->pos = 0;
    return io;
}

bool SDL_CloseIO(SDL_IOStream *context)
{
    if (!context) return SDL_FALSE;
    if (context->fp) fclose(context->fp);
    free(context);
    return SDL_TRUE;
}

void *SDL_LoadFile(const char *file, size_t *datasize)
{
    FILE *fp = fopen(file, "rb");
    if (!fp) { if (datasize) *datasize = 0; return NULL; }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void *buf = malloc(len > 0 ? (size_t)len : 1);
    if (len > 0) fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (datasize) *datasize = (size_t)len;
    return buf;
}

Uint8 *SDL_IOStreamReadAll(SDL_IOStream *context, size_t *datasize)
{
    if (!context) { if (datasize) *datasize = 0; return NULL; }
    Uint8 *buf = NULL;
    size_t len = 0;
    if (context->fp) {
        fseek(context->fp, 0, SEEK_END);
        long n = ftell(context->fp);
        fseek(context->fp, 0, SEEK_SET);
        if (n > 0) {
            buf = (Uint8 *)malloc((size_t)n);
            if (buf) { fread(buf, 1, (size_t)n, context->fp); len = (size_t)n; }
        }
    } else if (context->mem) {
        len = context->size;
        buf = (Uint8 *)malloc(len > 0 ? len : 1);
        if (buf && len > 0) memcpy(buf, context->mem, len);
    }
    if (datasize) *datasize = len;
    return buf;
}

/* Legacy SDL2 RWops */
struct SDL_RWops {
    FILE *fp;
    Uint8 *mem;
    int size;
    int pos;
};

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
    FILE *fp = fopen(file, mode);
    if (!fp) return NULL;
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(*rw));
    rw->fp = fp;
    return rw;
}

SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
    SDL_RWops *rw = (SDL_RWops *)calloc(1, sizeof(*rw));
    rw->mem = (Uint8 *)mem;
    rw->size = size;
    return rw;
}

void *SDL_ReadFile(const char *file, size_t *datasize)
{
    return SDL_LoadFile(file, datasize);
}

void SDL_FreeRW(SDL_RWops *rw)
{
    if (!rw) return;
    if (rw->fp) fclose(rw->fp);
    free(rw);
}

/* ------------------------------------------------------------------ */
/* Mutex (simple non-recursive lock)                                   */
/* ------------------------------------------------------------------ */
struct SDL_Mutex { volatile int locked; };

SDL_Mutex *SDL_CreateMutex(void)
{
    SDL_Mutex *m = (SDL_Mutex *)calloc(1, sizeof(*m));
    return m;
}

void SDL_DestroyMutex(SDL_Mutex *mutex) { free(mutex); }

void SDL_LockMutex(SDL_Mutex *mutex)
{
    if (!mutex) return;
    while (__atomic_test_and_set(&mutex->locked, __ATOMIC_ACQUIRE))
        rg_task_yield();
}

void SDL_UnlockMutex(SDL_Mutex *mutex)
{
    if (!mutex) return;
    __atomic_clear(&mutex->locked, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Audio stream (minimal no-op for now)                                */
/* ------------------------------------------------------------------ */
struct SDL_AudioStream { int freq; int channels; };

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, void (*callback)(void *, SDL_AudioStream *, int, int), void *userdata)
{
    (void)devid; (void)callback; (void)userdata;
    SDL_AudioStream *s = (SDL_AudioStream *)calloc(1, sizeof(*s));
    if (spec) { s->freq = spec->freq; s->channels = spec->channels; }
    return s;
}

bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *stream, void (*callback)(void *, SDL_AudioStream *, int, int), void *userdata)
{ (void)stream; (void)callback; (void)userdata; return SDL_TRUE; }

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len)
{ (void)stream; (void)buf; (void)len; return SDL_TRUE; }

bool SDL_GetAudioStreamDevice(SDL_AudioStream *stream, SDL_AudioDeviceID *devid)
{ (void)stream; if (devid) *devid = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK; return SDL_TRUE; }

bool SDL_PauseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; return SDL_TRUE; }
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev) { (void)dev; return SDL_TRUE; }

void SDL_DestroyAudioStream(SDL_AudioStream *stream) { free(stream); }

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
static SDL_LogPriority g_log_priority = SDL_LOG_PRIORITY_INFO;

void SDL_SetLogPriorities(SDL_LogPriority priority) { g_log_priority = priority; }

void SDL_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void SDL_LogMessage(int category, SDL_LogPriority priority, const char *fmt, ...)
{
    (void)category;
    if (priority < g_log_priority) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void SDL_LogWarn(int category, const char *fmt, ...)
{
    (void)category;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Legacy SDL2 entry points                                            */
/* ------------------------------------------------------------------ */
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags)
{
    (void)bpp; (void)flags;
    if (!g_window) {
        g_window = (SDL_Window *)calloc(1, sizeof(*g_window));
        g_window->w = width;
        g_window->h = height;
    }
    return SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);
}

static SDL_VideoInfo g_video_info;
const SDL_VideoInfo *SDL_GetVideoInfo(void)
{
    memset(&g_video_info, 0, sizeof(g_video_info));
    g_video_info.hw_available = 0;
    g_video_info.wm_available = 0;
    g_video_info.vfmt = &g_format_argb;
    return &g_video_info;
}

void SDL_WM_SetCaption(const char *title, const char *icon) { (void)title; (void)icon; }
void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 *mask) { (void)mask; if (icon) SDL_DestroySurface(icon); }
