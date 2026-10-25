/*
 * SDL3-compat: a minimal SDL3 API compatibility layer for retro-go (ESP32).
 *
 * This is NOT SDL3. It implements just enough of the SDL3 API surface that the
 * jysdl-bh engine (金庸群侠传 Lua 复刻版) uses, mapping it onto retro-go's
 * rg_surface / rg_display / rg_input / rg_audio primitives.
 *
 * All rendering is software rendering into 32-bit ARGB8888 buffers.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Basic types                                                         */
/* ------------------------------------------------------------------ */
typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t   Sint8;
typedef int16_t  Sint16;
typedef int32_t  Sint32;
typedef int64_t  Sint64;

#define SDL_FALSE 0
#define SDL_TRUE  1
typedef int SDL_bool;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define SDLCALL

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */
typedef struct SDL_Point { int x, y; } SDL_Point;
typedef struct SDL_FPoint { float x, y; } SDL_FPoint;
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_FRect { float x, y, w, h; } SDL_FRect;
typedef struct SDL_Color { Uint8 r, g, b, a; } SDL_Color;

typedef enum SDL_FlipMode {
    SDL_FLIP_NONE       = 0,
    SDL_FLIP_HORIZONTAL = 1,
    SDL_FLIP_VERTICAL   = 2
} SDL_FlipMode;

typedef enum SDL_BlendMode {
    SDL_BLENDMODE_NONE  = 0,
    SDL_BLENDMODE_BLEND = 1,
    SDL_BLENDMODE_ADD   = 2,
    SDL_BLENDMODE_MOD   = 4,
    SDL_BLENDMODE_MUL   = 5,
} SDL_BlendMode;

typedef enum SDL_ScaleMode {
    SDL_SCALEMODE_NEAREST = 0,
    SDL_SCALEMODE_LINEAR  = 1,
    SDL_SCALEMODE_BEST    = 2,
} SDL_ScaleMode;

/* ------------------------------------------------------------------ */
/* Pixel formats                                                       */
/* ------------------------------------------------------------------ */
typedef Uint32 SDL_PixelFormat;
#define SDL_PIXELFORMAT_UNKNOWN    0u
#define SDL_PIXELFORMAT_INDEX8     0x01030101u
#define SDL_PIXELFORMAT_RGB565     0x01040602u
#define SDL_PIXELFORMAT_ARGB8888   0x16361804u
#define SDL_PIXELFORMAT_RGBA8888   0x16762004u
#define SDL_PIXELFORMAT_ABGR8888   0x16761804u
#define SDL_PIXELFORMAT_BGRA8888   0x16862004u

typedef struct SDL_Palette {
    int ncolors;
    SDL_Color *colors;
    int version;
    int refcount;
} SDL_Palette;

typedef struct SDL_PixelFormatDetails {
    Uint32 format;
    int bits_per_pixel;
    int bytes_per_pixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormatDetails;

/* SDL2-style SDL_PixelFormat struct, used as `surface->format`. */
typedef struct SDL_PixelFormatSDL2 {
    Uint32 format;
    SDL_Palette *palette;
    Uint8 BitsPerPixel;
    Uint8 BytesPerPixel;
    Uint32 Rmask, Gmask, Bmask, Amask;
} SDL_PixelFormatSDL2;

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */
typedef struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormatSDL2 *format;   /* SDL2-compatible field the engine uses */
    int w, h;
    int pitch;                     /* bytes per row */
    void *pixels;
    SDL_Rect clip_rect;
    Uint32 color_key;              /* transparent color (colorkey) */
    bool has_color_key;
    Uint8 alpha_mod;               /* surface alpha 0..255 */
    bool free_pixels;
} SDL_Surface;

#define SDL_SWSURFACE   0x00000000
#define SDL_HWSURFACE   0x00000001
#define SDL_RLEACCEL    0x00000002
#define SDL_SRCCOLORKEY 0x00001000
#define SDL_PREALLOC    0x00000001

/* ------------------------------------------------------------------ */
/* Textures / renderer / window                                        */
/* ------------------------------------------------------------------ */
typedef struct SDL_Texture {
    Uint32 format;
    int access;
    int w, h;
    Uint32 *pixels;                /* ARGB8888, size w*h */
    SDL_BlendMode blend_mode;
    Uint8 alpha_mod;
    Uint8 r_mod, g_mod, b_mod;
    SDL_ScaleMode scale_mode;
    bool free_pixels;
} SDL_Texture;

typedef struct SDL_Window SDL_Window;

typedef struct SDL_Renderer {
    SDL_Window *window;
    SDL_Texture *target;           /* NULL = backbuffer */
    Uint8 draw_r, draw_g, draw_b, draw_a;
    SDL_BlendMode draw_blend;
    SDL_Rect clip_rect;
    bool has_clip;
    SDL_Texture *backbuffer;       /* logical window surface (g_ScreenW x g_ScreenH) */
} SDL_Renderer;

struct SDL_Window {
    int w, h;
    bool fullscreen;
};

typedef enum SDL_TextureAccess {
    SDL_TEXTUREACCESS_STATIC    = 0,
    SDL_TEXTUREACCESS_STREAMING = 1,
    SDL_TEXTUREACCESS_TARGET    = 2,
} SDL_TextureAccess;

/* ------------------------------------------------------------------ */
/* Gamepad / input                                                     */
/* ------------------------------------------------------------------ */
typedef Uint32 SDL_JoystickID;
typedef struct SDL_Gamepad SDL_Gamepad;

typedef enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_SOUTH          = 0,
    SDL_GAMEPAD_BUTTON_EAST           = 1,
    SDL_GAMEPAD_BUTTON_WEST           = 2,
    SDL_GAMEPAD_BUTTON_NORTH          = 3,
    SDL_GAMEPAD_BUTTON_BACK           = 4,
    SDL_GAMEPAD_BUTTON_GUIDE          = 5,
    SDL_GAMEPAD_BUTTON_START          = 6,
    SDL_GAMEPAD_BUTTON_LEFT_STICK     = 7,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK    = 8,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  = 9,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER = 10,
    SDL_GAMEPAD_BUTTON_DPAD_UP        = 11,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN      = 12,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT      = 13,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT     = 14,
    SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;

typedef enum SDL_GamepadAxis {
    SDL_GAMEPAD_AXIS_LEFTX = 0,
    SDL_GAMEPAD_AXIS_LEFTY = 1,
    SDL_GAMEPAD_AXIS_RIGHTX = 2,
    SDL_GAMEPAD_AXIS_RIGHTY = 3,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER = 4,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER = 5,
    SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;

typedef Sint32 SDL_Keycode;
typedef Uint32 SDL_Keymod;

#define SDLK_SCANCODE_MASK (1u << 30)

/* Letters (SDL3 aliases uppercase to lowercase) */
#define SDLK_a ((SDL_Keycode)'a')
#define SDLK_b ((SDL_Keycode)'b')
#define SDLK_c ((SDL_Keycode)'c')
#define SDLK_d ((SDL_Keycode)'d')
#define SDLK_e ((SDL_Keycode)'e')
#define SDLK_f ((SDL_Keycode)'f')
#define SDLK_g ((SDL_Keycode)'g')
#define SDLK_h ((SDL_Keycode)'h')
#define SDLK_i ((SDL_Keycode)'i')
#define SDLK_j ((SDL_Keycode)'j')
#define SDLK_k ((SDL_Keycode)'k')
#define SDLK_l ((SDL_Keycode)'l')
#define SDLK_m ((SDL_Keycode)'m')
#define SDLK_n ((SDL_Keycode)'n')
#define SDLK_o ((SDL_Keycode)'o')
#define SDLK_p ((SDL_Keycode)'p')
#define SDLK_q ((SDL_Keycode)'q')
#define SDLK_r ((SDL_Keycode)'r')
#define SDLK_s ((SDL_Keycode)'s')
#define SDLK_t ((SDL_Keycode)'t')
#define SDLK_u ((SDL_Keycode)'u')
#define SDLK_v ((SDL_Keycode)'v')
#define SDLK_w ((SDL_Keycode)'w')
#define SDLK_x ((SDL_Keycode)'x')
#define SDLK_y ((SDL_Keycode)'y')
#define SDLK_z ((SDL_Keycode)'z')
#define SDLK_A SDLK_a
#define SDLK_B SDLK_b
#define SDLK_C SDLK_c
#define SDLK_D SDLK_d
#define SDLK_E SDLK_e
#define SDLK_F SDLK_f
#define SDLK_G SDLK_g
#define SDLK_H SDLK_h
#define SDLK_I SDLK_i
#define SDLK_J SDLK_j
#define SDLK_K SDLK_k
#define SDLK_L SDLK_l
#define SDLK_M SDLK_m
#define SDLK_N SDLK_n
#define SDLK_O SDLK_o
#define SDLK_P SDLK_p
#define SDLK_Q SDLK_q
#define SDLK_R SDLK_r
#define SDLK_S SDLK_s
#define SDLK_T SDLK_t
#define SDLK_U SDLK_u
#define SDLK_V SDLK_v
#define SDLK_W SDLK_w
#define SDLK_X SDLK_x
#define SDLK_Y SDLK_y
#define SDLK_Z SDLK_z

#define SDLK_0 ((SDL_Keycode)'0')
#define SDLK_1 ((SDL_Keycode)'1')
#define SDLK_2 ((SDL_Keycode)'2')
#define SDLK_3 ((SDL_Keycode)'3')
#define SDLK_4 ((SDL_Keycode)'4')
#define SDLK_5 ((SDL_Keycode)'5')
#define SDLK_6 ((SDL_Keycode)'6')
#define SDLK_7 ((SDL_Keycode)'7')
#define SDLK_8 ((SDL_Keycode)'8')
#define SDLK_9 ((SDL_Keycode)'9')

#define SDLK_RETURN  ((SDL_Keycode)'\r')
#define SDLK_ESCAPE  ((SDL_Keycode)'\033')
#define SDLK_SPACE   ((SDL_Keycode)' ')

#define SDLK_RIGHT ((SDL_Keycode)0x4000004F)
#define SDLK_LEFT  ((SDL_Keycode)0x40000050)
#define SDLK_DOWN  ((SDL_Keycode)0x40000051)
#define SDLK_UP    ((SDL_Keycode)0x40000052)

#define SDLK_F1 ((SDL_Keycode)0x4000003A)
#define SDLK_F2 ((SDL_Keycode)0x4000003B)
#define SDLK_F3 ((SDL_Keycode)0x4000003C)
#define SDLK_F4 ((SDL_Keycode)0x4000003D)
#define SDLK_F5 ((SDL_Keycode)0x4000003E)

typedef enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_F1 = 58,
} SDL_Scancode;

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */
typedef enum SDL_EventType {
    SDL_EVENT_QUIT                   = 0x100,
    SDL_EVENT_KEY_DOWN               = 0x300,
    SDL_EVENT_KEY_UP                 = 0x301,
    SDL_EVENT_MOUSE_MOTION           = 0x400,
    SDL_EVENT_MOUSE_BUTTON_DOWN      = 0x401,
    SDL_EVENT_MOUSE_BUTTON_UP        = 0x402,
    SDL_EVENT_MOUSE_WHEEL            = 0x403,
    SDL_EVENT_GAMEPAD_AXIS_MOTION    = 0x650,
    SDL_EVENT_GAMEPAD_BUTTON_DOWN    = 0x651,
    SDL_EVENT_GAMEPAD_BUTTON_UP      = 0x652,
    SDL_EVENT_GAMEPAD_ADDED          = 0x653,
    SDL_EVENT_GAMEPAD_REMOVED        = 0x654,
} SDL_EventType;

typedef struct SDL_KeyboardEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    SDL_Keycode key;
    Uint8 scancode;
    bool down;
    bool repeat;
} SDL_KeyboardEvent;

typedef struct SDL_MouseMotionEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    float x, y;
    float xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct SDL_MouseButtonEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    Uint8 button;
    bool down;
    float x, y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseWheelEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    float x, y;
} SDL_MouseWheelEvent;

typedef struct SDL_GamepadAxisEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    Uint8 axis;
    Sint16 value;
} SDL_GamepadAxisEvent;

typedef struct SDL_GamepadButtonEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    Uint8 button;
    bool down;
} SDL_GamepadButtonEvent;

typedef struct SDL_GamepadDeviceEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
} SDL_GamepadDeviceEvent;

typedef union SDL_Event {
    SDL_EventType type;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_GamepadAxisEvent gaxis;
    SDL_GamepadButtonEvent gbutton;
    SDL_GamepadDeviceEvent gdevice;
    Uint8 padding[64];
} SDL_Event;

#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3

/* ------------------------------------------------------------------ */
/* IO                                                                  */
/* ------------------------------------------------------------------ */
typedef struct SDL_IOStream SDL_IOStream;

/* ------------------------------------------------------------------ */
/* Properties                                                          */
/* ------------------------------------------------------------------ */
typedef Uint32 SDL_PropertiesID;

#define SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER     "SDL.window.create.width"
#define SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER    "SDL.window.create.height"
#define SDL_PROP_WINDOW_CREATE_TITLE_STRING     "SDL.window.create.title"
#define SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN "SDL.window.create.resizable"
#define SDL_PROP_RENDERER_CREATE_WINDOW_POINTER "SDL.renderer.create.window"

/* ------------------------------------------------------------------ */
/* Audio (minimal; mostly stubbed because jysdl audio is reworked)     */
/* ------------------------------------------------------------------ */
typedef struct SDL_AudioStream SDL_AudioStream;
typedef struct SDL_AudioDevice SDL_AudioDevice;
typedef Uint32 SDL_AudioDeviceID;

typedef enum SDL_AudioFormat {
    SDL_AUDIO_UNKNOWN = 0,
    SDL_AUDIO_S8      = 0x8008,
    SDL_AUDIO_S16     = 0x8010,
    SDL_AUDIO_S32     = 0x8020,
    SDL_AUDIO_F32     = 0x8120,
} SDL_AudioFormat;

typedef struct SDL_AudioSpec {
    SDL_AudioFormat format;
    int channels;
    int freq;
} SDL_AudioSpec;

#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFF)

/* ------------------------------------------------------------------ */
/* Mutex                                                               */
/* ------------------------------------------------------------------ */
typedef struct SDL_Mutex SDL_Mutex;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
typedef enum SDL_LogPriority {
    SDL_LOG_PRIORITY_VERBOSE  = 1,
    SDL_LOG_PRIORITY_DEBUG    = 2,
    SDL_LOG_PRIORITY_INFO     = 3,
    SDL_LOG_PRIORITY_WARN     = 4,
    SDL_LOG_PRIORITY_ERROR    = 5,
    SDL_LOG_PRIORITY_CRITICAL = 6,
} SDL_LogPriority;

#define SDL_LOG_CATEGORY_APPLICATION 1

/* ------------------------------------------------------------------ */
/* Hints                                                               */
/* ------------------------------------------------------------------ */
#define SDL_HINT_RENDER_DRIVER        "SDL_RENDER_DRIVER"
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

/* ------------------------------------------------------------------ */
/* Init flags                                                          */
/* ------------------------------------------------------------------ */
typedef Uint32 SDL_InitFlags;
#define SDL_INIT_AUDIO   0x00000010u
#define SDL_INIT_VIDEO   0x00000020u
#define SDL_INIT_GAMEPAD 0x00002000u
#define SDL_INIT_EVENTS  0x00004000u

/* SDL2 legacy flags referenced by the engine */
#define SDL_FULLSCREEN   0x00000001
#define SDL_DOUBLEBUF    0x40000000
#define SDL_VIDEO_CENTERED 0
#define SDL_VIDEO_WINDOW_POS 0

typedef struct SDL_VideoInfo {
    Uint32 hw_available;
    Uint32 wm_available;
    Uint32 blit_hw;
    Uint32 blit_hw_CC;
    Uint32 blit_hw_A;
    Uint32 blit_sw;
    Uint32 blit_sw_CC;
    Uint32 blit_sw_A;
    Uint32 blit_fill;
    Uint32 video_mem;
    SDL_PixelFormatSDL2 *vfmt;
} SDL_VideoInfo;

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
void *SDL_malloc(size_t size);
void *SDL_calloc(size_t nmemb, size_t size);
void *SDL_realloc(void *mem, size_t size);
void  SDL_free(void *mem);

/* ------------------------------------------------------------------ */
/* Init / quit                                                         */
/* ------------------------------------------------------------------ */
int SDL_Init(SDL_InitFlags flags);
int SDL_InitSubSystem(SDL_InitFlags flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_ClearError(void);
int SDL_SetHint(const char *name, const char *value);
int SDL_setenv_unsafe(const char *name, const char *value, int overwrite);

/* ------------------------------------------------------------------ */
/* Video / window / renderer                                           */
/* ------------------------------------------------------------------ */
SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props);
void SDL_DestroyWindow(SDL_Window *window);
bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen);
bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon);
const char *SDL_VideoDriverName(char *namebuf, int maxlen);

SDL_Renderer *SDL_CreateRendererWithProperties(SDL_PropertiesID props);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
bool SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture);
bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
bool SDL_SetRenderDrawBlendMode(SDL_Renderer *renderer, SDL_BlendMode mode);
bool SDL_SetRenderClipRect(SDL_Renderer *renderer, const SDL_Rect *rect);
bool SDL_GetRenderClipRect(SDL_Renderer *renderer, SDL_Rect *rect);
bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect);
bool SDL_RenderLine(SDL_Renderer *renderer, float x1, float y1, float x2, float y2);
bool SDL_RenderTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect);
bool SDL_RenderTextureRotated(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *srcrect, const SDL_FRect *dstrect, double angle, const SDL_FPoint *center, SDL_FlipMode flip);
bool SDL_RenderPresent(SDL_Renderer *renderer);

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h);
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer *renderer, SDL_Surface *surface);
void SDL_DestroyTexture(SDL_Texture *texture);
bool SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode mode);
bool SDL_SetTextureAlphaMod(SDL_Texture *texture, Uint8 alpha);
bool SDL_SetTextureColorMod(SDL_Texture *texture, Uint8 r, Uint8 g, Uint8 b);
bool SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode mode);
bool SDL_GetTextureSize(SDL_Texture *texture, float *w, float *h);

/* ------------------------------------------------------------------ */
/* Surface                                                             */
/* ------------------------------------------------------------------ */
SDL_PixelFormat SDL_GetPixelFormatForMasks(int bpp, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateSurface(int w, int h, SDL_PixelFormat format);
SDL_Surface *SDL_CreateSurfaceFrom(int w, int h, SDL_PixelFormat format, void *pixels, int pitch);
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
void SDL_DestroySurface(SDL_Surface *surface);
SDL_Surface *SDL_ConvertSurface(SDL_Surface *surface, SDL_PixelFormatSDL2 *format);
Uint32 SDL_MapSurfaceRGBA(SDL_Surface *surface, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
bool SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, const SDL_Rect *dstrect);
bool SDL_FillSurfaceRect(SDL_Surface *dst, const SDL_Rect *rect, Uint32 color);
bool SDL_SetSurfaceColorKey(SDL_Surface *surface, bool enabled, Uint32 key);
bool SDL_SetSurfaceAlphaMod(SDL_Surface *surface, Uint8 alpha);
bool SDL_SetSurfaceClipRect(SDL_Surface *surface, const SDL_Rect *rect);
bool SDL_SetPaletteColors(SDL_Palette *palette, const SDL_Color *colors, int firstcolor, int ncolors);
bool SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);

/* ------------------------------------------------------------------ */
/* Input / events / gamepad                                            */
/* ------------------------------------------------------------------ */
bool SDL_PollEvent(SDL_Event *event);
bool SDL_PushEvent(SDL_Event *event);
void SDL_SetEventFilter(bool (*filter)(void *userdata, SDL_Event *event), void *userdata);
void SDL_SetGamepadEventsEnabled(bool enabled);

const Uint8 *SDL_GetKeyboardState(int *numkeys);
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, SDL_Keymod *modstate);

SDL_JoystickID *SDL_GetJoysticks(int *count);
bool SDL_IsGamepad(SDL_JoystickID instance_id);
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id);
void SDL_CloseGamepad(SDL_Gamepad *gamepad);

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
Uint64 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

/* ------------------------------------------------------------------ */
/* IO                                                                  */
/* ------------------------------------------------------------------ */
SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode);
SDL_IOStream *SDL_IOFromMem(void *mem, size_t size);
bool SDL_CloseIO(SDL_IOStream *context);
void *SDL_LoadFile(const char *file, size_t *datasize);
/* Internal helper (retro-go port): read an entire IO stream into a malloc'd buffer. */
Uint8 *SDL_IOStreamReadAll(SDL_IOStream *context, size_t *datasize);

/* Legacy SDL2 IO (SDL_RWops), used by SDL_image shim */
typedef struct SDL_RWops SDL_RWops;
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode);
SDL_RWops *SDL_RWFromMem(void *mem, int size);
void *SDL_ReadFile(const char *file, size_t *datasize);
void SDL_FreeRW(SDL_RWops *rw);

/* ------------------------------------------------------------------ */
/* Properties                                                          */
/* ------------------------------------------------------------------ */
SDL_PropertiesID SDL_CreateProperties(void);
bool SDL_DestroyProperties(SDL_PropertiesID props);
bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value);
bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value);
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value);
bool SDL_SetFloatProperty(SDL_PropertiesID props, const char *name, float value);
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value);
void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value);
const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value);
Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value);
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);

/* ------------------------------------------------------------------ */
/* Mutex                                                               */
/* ------------------------------------------------------------------ */
SDL_Mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_Mutex *mutex);
void SDL_LockMutex(SDL_Mutex *mutex);
void SDL_UnlockMutex(SDL_Mutex *mutex);

/* ------------------------------------------------------------------ */
/* Audio stream (minimal)                                              */
/* ------------------------------------------------------------------ */
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec, void (*callback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount), void *userdata);
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *stream, void (*callback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount), void *userdata);
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len);
bool SDL_GetAudioStreamDevice(SDL_AudioStream *stream, SDL_AudioDeviceID *devid);
bool SDL_PauseAudioDevice(SDL_AudioDeviceID dev);
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID dev);
void SDL_DestroyAudioStream(SDL_AudioStream *stream);

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
void SDL_Log(const char *fmt, ...);
void SDL_LogMessage(int category, SDL_LogPriority priority, const char *fmt, ...);
void SDL_LogWarn(int category, const char *fmt, ...);
void SDL_SetLogPriorities(SDL_LogPriority priority);

/* ------------------------------------------------------------------ */
/* Legacy SDL2 entry points referenced (some in commented code)        */
/* ------------------------------------------------------------------ */
SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags);
const SDL_VideoInfo *SDL_GetVideoInfo(void);
void SDL_WM_SetCaption(const char *title, const char *icon);
void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 *mask);

#ifdef __cplusplus
}
#endif
