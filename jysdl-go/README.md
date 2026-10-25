# jysdl-go — 金庸群侠传 (jysdl-bh) port for Retro-Go / ESP32-P4

This app ports [scarsty/jysdl-bh](https://github.com/scarsty/jysdl-bh)（金庸群侠传 Lua 复刻版）into the
Retro-Go multi-binary firmware as a new application. It builds on the existing Retro-Go application
framework (`launcher` + per-app OTA partitions), following the same layout as `prboom-go`,
`video-player`, etc.

## Status

**The app compiles and links for `esp32-p4-devkit`.** The produced `jysdl-go.bin` is ~800 KB and fits
comfortably inside its 2 MB partition.

This is a **foundation port**: the engine, Lua interpreter and an SDL3→Retro-Go compatibility layer are
all wired up and buildable, but several desktop subsystems are still stubbed (see below). A playable
build additionally requires the game's data files (Lua scripts + art + audio), which are *not* part of
`jysdl-bh` and must be supplied separately.

## Layout

```
jysdl-go/
├── CMakeLists.txt                 # app entry (COMPONENTS "main retro-go jysdl")
├── sdkconfig                      # esp32p4 config (copied from launcher)
├── main/
│   ├── CMakeLists.txt
│   └── main.c                     # app_main() -> rg_system_init -> jysdl_main()
└── components/jysdl/
    ├── CMakeLists.txt
    ├── src/                       # vendored jysdl-bh engine (.cpp/.h, miniz.h, tsf.h, tml.h)
    ├── lua/                       # vendored Lua 5.4.x
    ├── libs/lodepng/              # PNG decoder (from retro-go)
    ├── smallpot/                  # PotDll.h
    └── sdl-compat/                # SDL3 -> retro-go compatibility layer
        ├── SDL3/SDL.h             #   SDL3 API subset (software ARGB8888 renderer)
        ├── SDL3_image/            #   IMG_* via lodepng
        ├── SDL3_ttf/              #   TTF_* stub
        ├── SDL3_mixer/            #   empty placeholder
        ├── sdlcompat.cpp          #   renderer/input/timing/IO implementation
        ├── sdl_image.cpp          #   PNG loading via lodepng
        └── sdl_ttf.cpp            #   text placeholder
```

## Build

```bash
cd /path/to/retro-go-master
source /path/to/esp-idf/export.sh
# Build just this app:
python rg_tool.py --target esp32-p4-devkit --no-networking build jysdl-go
# Build a full image (launcher + jysdl-go):
python rg_tool.py --target esp32-p4-devkit --no-networking build-img launcher jysdl-go
```

## What is implemented

- **SDL3 rendering** (`SDL_Window`/`SDL_Renderer`/`SDL_Texture`/`SDL_Surface`) as a software
  ARGB8888 renderer backed by `rg_surface` + `rg_display_submit`; supports blit with scaling,
  `SDL_RenderTextureRotated`, fill-rect/line, clip rect, blend modes (none/blend/add), and
  `SDL_RenderPresent` with 320×240 downscale.
- **Input**: retro-go gamepad (`rg_input_read_gamepad`) synthesized into SDL3 gamepad events; the
  engine's existing `JY_GetKey` maps them to the game's keycodes. `MENU`/`OPTION` open Retro-Go menus.
- **Timing / IO / properties / mutex / logging** shims.
- **Lua 5.4** interpreter (vendored, compiled with `LUA_C89_NUMBERS`).
- **PNG image loading** via `lodepng`.
- **Build integration**: registered in `rg_tool.py` (`PROJECT_APPS`) and the launcher
  (`launcher/main/applications.c`).

## Stubbed / remaining work (in priority order)

1. **Audio** — `Audio.cpp` is a no-op. A real implementation should use `rg_audio_submit()` with
   TinySoundFont (`tsf.h`/`tml.h`, already bundled) for MIDI (needs an `.sf2` soundfont, see
   `g_MidSF2`) plus a simple WAV mixer for sound effects.
2. **Text rendering** — now renders via retro-go's built-in `font_FusionPixel` (12px, 3950 common
   CJK glyphs), scaled + colored into the framebuffer (`jysdl_font.cpp`). Covers common Chinese;
   rare/less-common characters render blank. FreeType + `font/bkai00mp.ttf` (in jysdl_data) is the
   optional fallback for full coverage.
3. **Charset conversion** — GBK/Big5→Unicode now uses `hzmb.dat` via `JY_CharSet()`/`LoadMB()`
   (already in the engine). Simplified↔traditional via `SimpleCC` still needs the ST/TS phrase files.
4. **ZIP support** — `ZipFile` (libzip) is stubbed. Use `miniz` (bundled) to implement
   `readFile`/`getFileNames`/`zip`/`unzip`.
5. **Game data** — the actual game (Lua scripts, PNG/WebP art, MIDI/WAV, fonts, `config.lua`,
   `index.ka`/`index.txt`) is not in `jysdl-bh` and must be supplied separately. Put it on the SD
   card under `/sd/roms/jy/` (either flat or in a subdirectory), then select any `.lua` file from
   the launcher's *Jin Yong Hero* tab — the app uses that file's directory as the game root and
   loads `config.lua` from it.
6. **Resolution** — the engine renders at `config.lua`'s `Width`×`Height` and `SDL_RenderPresent`
   nearest-scales to 320×240; a proper aspect-correct / letterboxed scaler is a nice-to-have.
7. **Clean exit** — the engine's `exit(1)` quit paths should be routed through `rg_system_exit()`.
8. **Performance** — Lua + software blitting + (eventual) TinySoundFont all need profiling/tuning on
   the ESP32-P4.

## License note

`jysdl-bh` is derived from the 金庸群侠传 Lua 复刻版 code (originally by 游泳的鱼, extended by
蓝烟清 / scarsty). Its license is inherited from that project; the game data and original game assets
are separate and not included here.
