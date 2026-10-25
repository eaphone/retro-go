/*
 * jysdl_font.cpp — Chinese text rendering for the jysdl-bh engine using
 * retro-go's built-in FusionPixel bitmap font (RG_FONT_FUSIONPIXEL_12) instead
 * of SDL_ttf/FreeType.
 *
 * GBK/Big5 -> Unicode conversion reuses the engine's JY_CharSet()/LoadMB()
 * (which reads hzmb.dat, shipped in soarqin/jysdl_data). Each Unicode codepoint
 * is looked up in the FusionPixel glyph table and scaled/colored into the
 * engine's ARGB8888 render target (g_Texture).
 */
#include "charset.h"
#include "jymain.h"

extern "C" {
#include <rg_gui.h>
}
extern "C" const rg_font_t font_JYGBK12;

/* Direct-mapped glyph cache: the linear scan over ~3950 glyphs is by far the
 * dominant cost of JY_DrawStr (the game redraws the same text every frame). */
#define FUSION_GLYPH_CACHE_BITS 9
#define FUSION_GLYPH_CACHE_SIZE (1 << FUSION_GLYPH_CACHE_BITS)

static struct {
    uint16_t code;
    const rg_font_glyph_t *glyph;
} fusion_glyph_cache[FUSION_GLYPH_CACHE_SIZE];

static uint32_t fusion_hash(uint16_t code)
{
    return (code * 2654435761u) >> (32 - FUSION_GLYPH_CACHE_BITS);
}

static const rg_font_glyph_t *fusion_find_glyph(uint16_t codepoint)
{
    uint32_t slot = fusion_hash(codepoint);
    if (fusion_glyph_cache[slot].code == codepoint)
        return fusion_glyph_cache[slot].glyph;

    const uint8_t *ptr = font_JYGBK12.data;
    const rg_font_glyph_t *glyph = (const rg_font_glyph_t *)ptr;
    while (glyph->code && glyph->code != codepoint)
    {
        if (glyph->width != 0)
            ptr += (((glyph->width * glyph->height) - 1) / 8) + 1;
        ptr += sizeof(rg_font_glyph_t);
        glyph = (const rg_font_glyph_t *)ptr;
    }
    const rg_font_glyph_t *found = (glyph && glyph->code == codepoint) ? glyph : NULL;
    fusion_glyph_cache[slot].code = codepoint;
    fusion_glyph_cache[slot].glyph = found;
    return found;
}

static bool fusion_glyph_bit(const rg_font_glyph_t *g, int x, int y)
{
    int bit = x + y * g->width;
    return (g->data[bit / 8] & (0x80 >> (bit % 8))) != 0;
}

static void fusion_draw_glyph(const rg_font_glyph_t *g, int x, int y, int size, Uint32 color)
{
    SDL_Texture *dst = g_Texture;
    if (!g || !dst || !dst->pixels)
        return;

    int fh = font_JYGBK12.height;  /* 12 */
    int out_w = (g->width * size + fh / 2) / fh;
    int out_h = (g->height * size + fh / 2) / fh;
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    int ox = (g->xOffset * size + fh / 2) / fh;
    int oy = (g->yOffset * size + fh / 2) / fh;
    if (oy + out_h > size)
        oy = size - out_h;   /* clamp descenders to the baseline */

    for (int dy = 0; dy < out_h; dy++)
    {
        int sy = dy * g->height / out_h;
        for (int dx = 0; dx < out_w; dx++)
        {
            int sx = dx * g->width / out_w;
            if (!fusion_glyph_bit(g, sx, sy))
                continue;
            int px = x + ox + dx, py = y + oy + dy;
            if (px >= 0 && py >= 0 && px < dst->w && py < dst->h)
                dst->pixels[py * dst->w + px] = color;
        }
    }
}

static int utf8_to_utf16le(const char *str, char *out, size_t max_codepoints)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p && n < max_codepoints)
    {
        unsigned c = *p++;
        int extra = 0;
        if (c >= 0xF0)      { c &= 0x07; extra = 3; }
        else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
        else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
        else if (c >= 0x80) { c = '?'; }      /* stray continuation byte */
        for (int i = 0; i < extra; i++)
            c = (c << 6) | (*p++ & 0x3F);
        ((Uint16 *)out)[n++] = (Uint16)(c > 0xFFFF ? '?' : c);
    }
    ((Uint16 *)out)[n] = 0;
    return 0;
}

int JY_DrawStr(int x, int y, const char *str, int color, int size, const char *fontname,
    int charset, int OScharset)
{
    (void)fontname;
    (void)OScharset;

    if (str == NULL || strlen(str) == 0)
        return 0;
    if (strlen(str) > 127)
        return 0;

    char tmp[512];
    if (charset == 1)       /* Big5 -> Unicode */
        JY_CharSet(str, tmp, 2);
    else if (charset == 0)  /* GBK -> Unicode */
        JY_CharSet(str, tmp, 3);
    else                    /* already UTF-8 */
        utf8_to_utf16le(str, tmp, 255);

    Uint32 c = 0xFF000000u | (Uint32)(color & 0x00FFFFFF);
    Uint32 shadow = 0xFF000000u | (Uint32)((color >> 1) & 0x007F7F7F);

    Uint16 *u = (Uint16 *)tmp;
    int pen_x = x;
    for (int i = 0; i < 256; i++)
    {
        if (u[i] == 0)
            break;
        Uint16 ch = JY_TradToSimp_Char(u[i]);
        int advance = (ch < 128) ? ((size / 2) > 0 ? (size / 2) : size) : size;
        const rg_font_glyph_t *g = fusion_find_glyph(ch);
        if (g)
        {
            fusion_draw_glyph(g, pen_x + 1, y + 1, size, shadow);
            fusion_draw_glyph(g, pen_x, y, size, c);
        }
        pen_x += advance;
    }

    return 0;
}
