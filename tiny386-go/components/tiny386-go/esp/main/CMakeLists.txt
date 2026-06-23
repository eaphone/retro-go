# Standalone ESP-IDF main component + tiny386 core sources

# Parent directory containing core tiny386 sources
set(TINY386_DIR "${COMPONENT_DIR}/..")

idf_component_register(
    SRCS
        # ESP port sources
        "esp_main.c"
        "menu.c"
        "vkeyboard.c"
        "storage.c"
        "input.c"
        "wifi.c"
        "i2s.c"
        "sdmmc_crc_wrap.c"
        "lcd_ili9341.c"
        "lcd_axs15231b.c"
        "lcd_elecrow7s3.c"
        "lcd_st7701.c"
        # Core tiny386 emulator sources (parent directory)
        "../../i386.c"
        "../../pc.c"
        "../../vga.c"
        "../../ini.c"
        "../../i8042.c"
        "../../i8259.c"
        "../../i8254.c"
        "../../ide.c"
        "../../misc.c"
        "../../fpu.c"
        "../../i8257.c"
        "../../pci.c"
        "../../ne2000.c"
        "../../sb16.c"
        "../../fmopl.c"
        "../../adlib.c"
        "../../pcspk.c"
    INCLUDE_DIRS "." "${TINY386_DIR}"
)

# Global component compile options (applied AFTER ESP-IDF defaults)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${COMPONENT_DIR}/board_${BOARD}.h"
    -Wno-all
    -Wno-error=format
)

# O3 for performance-critical files (+ optional TINY386_NO_IRAM for ESP32-P4)
if(IDF_TARGET STREQUAL "esp32p4")
    set_source_files_properties("${TINY386_DIR}/i386.c" PROPERTIES COMPILE_FLAGS "-O3 -DTINY386_NO_IRAM")
    set_source_files_properties("${TINY386_DIR}/vga.c" PROPERTIES COMPILE_FLAGS "-O3 -DTINY386_NO_IRAM")
else()
    set_source_files_properties("${TINY386_DIR}/i386.c" PROPERTIES COMPILE_FLAGS "-O3")
    set_source_files_properties("${TINY386_DIR}/vga.c" PROPERTIES COMPILE_FLAGS "-O3")
endif()
set_source_files_properties("${TINY386_DIR}/pc.c" PROPERTIES COMPILE_FLAGS "-O3")

# fmopl needs -fsingle-precision-constant for ROM size
set_source_files_properties("${TINY386_DIR}/fmopl.c" PROPERTIES COMPILE_FLAGS "-fsingle-precision-constant")

# Feature defines (matching retro-go component)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    -DI386_ENABLE_FPU
    -DUSE_FMOPL
)
