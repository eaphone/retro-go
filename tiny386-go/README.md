# tiny386-go - IBM PC/XT/AT Emulator for Retro-Go

This is the **tiny386** PC emulator by superzazu, integrated as a Retro-Go app.

tiny386 emulates 8086/80186/386 processors with:
- CGA, EGA, VGA, SVGA video (up to 1280x1024)
- AdLib / Sound Blaster 16 / PC speaker audio
- NE2000 network card (via WiFi)
- IDE hard disk / floppy / CD-ROM
- Optional 80-bit FPU (i387)
- BIOS: SeaBIOS, FreeDOS ROM, custom ROMs

## Building

### Prerequisites
1. Build retro-go first (see retro-go README)
2. Run \make prepare\ in the tiny386 source directory to generate \mopl.inc\

### Build command
\\\ash
cd retro-go-master/tiny386-go
idf.py -B build -DBOARD=esp32p4 build
\\\

Replace \esp32p4\ with your board name (see boards in components/tiny386-go/esp/main/)

### Build tiny386 as standalone binary.
tiny386 could be run as standalone binary without retro-go.
\\\ash
make prepare
cd esp
idf.py -DBOARD=esp32p4 update-dependencies build
\\\

## Configuration

Place \	iny386.ini\ as \/sd/roms/dos/.system/tiny386.ini/\ or select it from the file browser.

## Supported Boards

| Board name | Description |
|-----------|-------------|
| esp32p4 | ESP32-P4-DevKitC with ILI9341 LCD (320x240) |
| esp32s3 | ESP32-S3-DevKitC with ILI9341 LCD (320x240) |
| jc3248w535 | JC3248W535 LCD |
| elecrow7s3 | Elecrow 7" S3 display |
| jc4880p443 | JC4880P443 LCD |

## Credits

- tiny386 by superzazu (https://github.com/superzazu/tiny386)
- Retro-Go by ducalex (https://github.com/ducalex/retro-go)
