# jysdl component: the jysdl-bh engine (金庸群侠传 Lua 复刻版) + Lua + SDL3-compat.
set(COMPONENT_SRCDIRS
    "src"
    "lua"
    "sdl-compat"
    "libs/lodepng"
)

set(COMPONENT_ADD_INCLUDEDIRS
    "src"
    "lua"
    "sdl-compat"
    "sdl-compat/SDL3"
    "sdl-compat/SDL3_image"
    "sdl-compat/SDL3_ttf"
    "sdl-compat/SDL3_mixer"
    "smallpot/include"
    "libs/lodepng"
)

set(COMPONENT_REQUIRES "retro-go")
register_component()

rg_setup_compile_options(
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-reorder
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
    -Wno-char-subscripts
    -Wno-misleading-indentation
)
