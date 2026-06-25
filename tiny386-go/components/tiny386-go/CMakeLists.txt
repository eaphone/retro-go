# tiny386-go component - a retro-go wrapper for the tiny386 PC emulator
set(COMPONENT_SRCS
    # Core tiny386 emulator sources
    "i386.c" "pc.c" "vga.c" "ini.c" "i8042.c"
    "i8259.c" "i8254.c" "ide.c" "misc.c" "fpu.c"
    "i8257.c" "pci.c" "ne2000.c" "sb16.c" "fmopl.c"
    "adlib.c" "pcspk.c"
    # ESP port sources
    "esp/main/tiny386_main.c"
    "esp/main/menu.c"
    "esp/main/vkeyboard.c"
    "esp/main/storage.c"
    "esp/main/input.c"
    "esp/main/wifi.c"
    "esp/main/i2s.c"
    "esp/main/sdmmc_crc_wrap.c"
    "esp/main/lcd_ili9341.c"
    "esp/main/lcd_axs15231b.c"
    "esp/main/lcd_elecrow7s3.c"
    "esp/main/lcd_st7701.c"
)
set(COMPONENT_ADD_INCLUDEDIRS "." "esp/main")
set(COMPONENT_REQUIRES "retro-go vfs")

register_component()

# Core tiny386 sources
# ESP32-P4 has very limited sram_low (~179KB), CPU emulator must stay in flash
if(IDF_TARGET STREQUAL "esp32p4")
    set(BOARD "esp32p4")
    set(TINY386_IRAM_FLAGS "-DTINY386_NO_IRAM")
else()
    set(BOARD "esp32s3")
    set(TINY386_IRAM_FLAGS "")
endif()
# Board selection: -DBOARD=<name> via cmake/idf.py, or BOARD env var, or default esp32s3.

message(STATUS "tiny386-go building for BOARD=")

# Force-include the board config header for all source files in this component
add_definitions(-include "${CMAKE_CURRENT_SOURCE_DIR}/esp/main/board_${BOARD}.h")

set_source_files_properties("adlib.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("fmopl.c" PROPERTIES COMPILE_FLAGS "-Wno-all -fsingle-precision-constant")
set_source_files_properties("fpu.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("i386.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3 ${TINY386_IRAM_FLAGS}")
set_source_files_properties("i8042.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("vga.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3 ${TINY386_IRAM_FLAGS}")
set_source_files_properties("pc.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("ne2000.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("sb16.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("i8257.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("ide.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("pci.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("i8259.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("i8254.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("pcspk.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("ini.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("misc.c" PROPERTIES COMPILE_FLAGS "-Wno-all")

# ESP port sources (adapted for retro-go)
set_source_files_properties("esp/main/tiny386_main.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/i2s.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/storage.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/input.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/menu.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/vkeyboard.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/wifi.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/sdmmc_crc_wrap.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/lcd_ili9341.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/lcd_axs15231b.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/lcd_elecrow7s3.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("esp/main/lcd_st7701.c" PROPERTIES COMPILE_FLAGS "-Wno-all")

# Add definitions
add_definitions(-DRETRO_GO=1)
add_definitions(-DI386_ENABLE_FPU)
add_definitions(-DUSE_FMOPL)

# Find the fmopl.inc in the component source directory
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmopl.inc")
    message(FATAL_ERROR "Please run 'make prepare' first in the tiny386 source directory. See README for details.")
endif()
