add_library(disphstx STATIC
    ${CMAKE_CURRENT_LIST_DIR}/disphstx.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_picolibsk.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vga.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vga_render.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vga_m33.S
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_dvi.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_dvi_render.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_dvi_m33.S
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode_simple.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode_format.c
    ${CMAKE_CURRENT_LIST_DIR}/disphstx_vmode_time.c
)

target_compile_definitions(disphstx PUBLIC
    USE_DISPHSTX=1
    DISPHSTX_PICOSDK=1
    DISPHSTX_USE_DVI=1
    DISPHSTX_USE_VGA=1
    USE_DRAWCAN=0
    USE_TEXT=0
    USE_RAND=0
    DISPHSTX_DVI_PINOUT=0
)

target_include_directories(disphstx PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(disphstx PUBLIC
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_gpio
    hardware_dma
    hardware_irq
    hardware_pll
    hardware_vreg
)

target_compile_options(disphstx PRIVATE
    -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter
    -Wno-unused-but-set-variable -Wno-implicit-function-declaration
    -Wno-sign-compare -Wno-incompatible-pointer-types
    -Wno-int-conversion -Wno-pointer-sign
)
