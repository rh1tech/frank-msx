cmake_minimum_required(VERSION 3.13)

# -----------------------------------------------------------------------------
# Platform selection
# -----------------------------------------------------------------------------
# Supported platforms (matches murmnes):
#   M2 — Murmulator 2.0                 HDMI_HSTX / HDMI_PIO / VGA / TV
#   M1 — Murmulator 1.x                 HDMI_PIO / VGA / TV
#   DV — Pimoroni Pico DV Demo          HDMI_PIO / VGA
#   PC — Olimex RP2040-PICO-PC          HDMI_PIO / VGA
#   Z0 — Waveshare RP2350-PiZero        HDMI_PIO / VGA
#
# Select with -DPLATFORM=m1|m2|dv|pc|z0 (default m2).
# BOARD_VARIANT is kept as a legacy alias for the same choice.
set(PLATFORM       "m2" CACHE STRING "Target platform: m1, m2, dv, pc, z0")
set_property(CACHE PLATFORM PROPERTY STRINGS m1 m2 dv pc z0)

# Allow BOARD_VARIANT (uppercase) as a back-compat alias.
if(DEFINED BOARD_VARIANT)
    string(TOLOWER "${BOARD_VARIANT}" _board_variant_lower)
    if(_board_variant_lower MATCHES "^(m1|m2|dv|pc|z0)$")
        set(PLATFORM "${_board_variant_lower}" CACHE STRING "" FORCE)
    endif()
endif()

string(TOUPPER "${PLATFORM}" BOARD_VARIANT)

# Z0 needs a non-standard Pico board header so the SDK configures the
# right flash, default pins, and RP2350B package detection. All other
# platforms use the stock pico2 definition.
if(PLATFORM STREQUAL "z0")
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Pico board type" FORCE)
else()
    set(PICO_BOARD "pico2" CACHE STRING "Pico board type")
endif()

# -----------------------------------------------------------------------------
# Video output driver selection
# -----------------------------------------------------------------------------
# Driver options (mutually exclusive):
#   HDMI_HSTX        RP2350 HSTX HDMI (with HDMI audio via data islands)
#   VIDEO_COMPOSITE  Software composite TV (PAL/NTSC) on HDMI DAC pins
#   (default)        PIO HDMI + runtime VGA autodetect
#
# The matrix below validates the user's choice against the platform —
# HDMI_HSTX is M2-only, VIDEO_COMPOSITE is M1/M2-only.
option(HDMI_HSTX       "Use the RP2350 HSTX-based HDMI driver (with HDMI audio)" OFF)
option(VIDEO_COMPOSITE "Use the software composite-TV driver"                    OFF)

if(HDMI_HSTX AND VIDEO_COMPOSITE)
    message(FATAL_ERROR "HDMI_HSTX and VIDEO_COMPOSITE are mutually exclusive")
endif()

if(HDMI_HSTX AND NOT PLATFORM STREQUAL "m2")
    message(FATAL_ERROR "HDMI_HSTX is only supported on the M2 platform (got '${PLATFORM}')")
endif()

if(VIDEO_COMPOSITE AND NOT (PLATFORM STREQUAL "m2" OR PLATFORM STREQUAL "m1"))
    message(FATAL_ERROR "VIDEO_COMPOSITE (composite TV) is only supported on M1/M2 (got '${PLATFORM}')")
endif()

# CPU / PSRAM / FLASH clocking (MHz). Defaults chosen for reliability.
# Composite-TV needs ~378 MHz: the TV scanline callback fires via a
# flash-resident alarm-pool dispatcher at 30 kHz, and at 252 MHz the
# dispatch overhead alone consumes most of each ~32 µs window — fMSX
# drops to ~1% speed. HDMI and VGA use direct DMA IRQs (pure SRAM path)
# so they run fine at 252 MHz. This matches murmnes's TV default.
if(VIDEO_COMPOSITE)
    set(_CPU_SPEED_DEFAULT "378")
else()
    set(_CPU_SPEED_DEFAULT "252")
endif()
set(CPU_SPEED "${_CPU_SPEED_DEFAULT}" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash target max frequency in MHz")

# MSX model: 1 = MSX1, 2 = MSX2, 3 = MSX2+
set(MSX_MODEL "3" CACHE STRING "MSX model to emulate (1, 2, or 3)")

# Optional USB HID host (disabled by default since we focus on PS/2)
option(USB_HID_ENABLED "Enable USB HID host" OFF)

# CPU voltage selection based on speed. The RP2350 vreg only exposes
# 1.50 V → 1.60 V → 1.65 V → 1.70 V steps (no 1.55 V).
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 300)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "Platform: ${PLATFORM}  CPU: ${CPU_SPEED} MHz  PSRAM: ${PSRAM_SPEED} MHz  Flash: ${FLASH_SPEED} MHz  Voltage: ${CPU_VOLTAGE}")

include(pico_sdk_import.cmake)

project(frank-msx C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Version string from version.txt ("MAJOR MINOR" -> "MAJOR.MINOR")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" FRANK_MSX_VERSION_RAW)
string(STRIP "${FRANK_MSX_VERSION_RAW}" FRANK_MSX_VERSION_RAW)
string(REPLACE " " "." FRANK_MSX_VERSION_STR "${FRANK_MSX_VERSION_RAW}")

#-----------------------------------------------------------------------------
# Driver libraries
#-----------------------------------------------------------------------------
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/nespad)
add_subdirectory(drivers/ps2)
add_subdirectory(drivers/usbhid)

#-----------------------------------------------------------------------------
# HSTX HDMI driver (copied verbatim from murmnes/frank-nes pico_hdmi/)
#-----------------------------------------------------------------------------
if(HDMI_HSTX)
    add_subdirectory(drivers/pico_hdmi)
    add_subdirectory(drivers/i2s)
endif()

# Video + audio + PSRAM live in a single "drivers" library. The video
# driver source swaps depending on the selected backend:
#   HDMI_HSTX=ON        → HDMI_hstx.c + pico_hdmi/i2s subdirectories
#   VIDEO_COMPOSITE=ON  → HDMI_tv.c + drivers/tv (software composite)
#   (default)           → HDMI.c + hdmi_scanline.S + HDMI_vga.c + test_pins.c
#                         with runtime HDMI-vs-VGA autodetect
if(HDMI_HSTX)
    set(_VIDEO_DRIVER_SRC drivers/HDMI_hstx.c)
elseif(VIDEO_COMPOSITE)
    set(_VIDEO_DRIVER_SRC drivers/HDMI_tv.c)
else()
    set(_VIDEO_DRIVER_SRC
        drivers/HDMI.c
        drivers/hdmi_scanline.S
        drivers/HDMI_vga.c
        drivers/test_pins.c
    )
endif()

#-----------------------------------------------------------------------------
# Software composite-TV driver (ported verbatim from murmnes/drivers/tv/).
# Symbols are textually renamed to tv_* via tv/tv_rename.h so they don't
# collide with the frank-msx HDMI.h API; HDMI_tv.c wraps the tv_* symbols.
#-----------------------------------------------------------------------------
if(VIDEO_COMPOSITE)
    add_library(tv_driver STATIC
        drivers/tv/tv-software.c
        drivers/tv/graphics.c
    )
    target_include_directories(tv_driver PUBLIC drivers drivers/tv)
    # Rename every public TV-driver symbol to a tv_* prefix so they
    # don't collide with frank-msx's HDMI.h API; and replace memset /
    # memcpy with inline SRAM-resident fast paths so the 30 kHz
    # scanline ISR doesn't thrash the XIP cache (fMSX drops to ~1 fps
    # otherwise).  Passed through "SHELL:" so CMake doesn't de-dupe
    # the repeated -include flags.
    # C-only: two -include headers (rename + SRAM memset/memcpy), plus
    # the existing warning-noise suppressors. We apply the -include
    # flags per-source instead of via target_compile_options because
    # CMake's SHELL: + generator-expression combo doesn't preserve the
    # "-include path" argument pair reliably.
    set(_tv_include_flags
        "-include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_rename.h"
        "-include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_mem.h"
    )
    string(JOIN " " _tv_include_flags_str ${_tv_include_flags})
    set_source_files_properties(
        drivers/tv/tv-software.c
        drivers/tv/graphics.c
        PROPERTIES
        COMPILE_FLAGS "${_tv_include_flags_str}"
    )
    target_compile_options(tv_driver PRIVATE
        -Wno-error
        -Wno-incompatible-pointer-types
        -Wno-all
        # GCC's loop-idiom pass rewrites our hand-inlined fills back into
        # memset()/memcpy() calls, defeating the whole point of tv_mem.h.
        # Disable the two passes that do this so the inline SRAM loops
        # survive to object code.
        -fno-tree-loop-distribute-patterns
        -fno-builtin-memset
        -fno-builtin-memcpy
    )
    target_link_libraries(tv_driver
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_clocks
    )
    if(PLATFORM STREQUAL "m2")
        target_compile_definitions(tv_driver PUBLIC TV_BASE_PIN=12)
    elseif(PLATFORM STREQUAL "m1")
        target_compile_definitions(tv_driver PUBLIC TV_BASE_PIN=6)
    endif()
    # Pass the platform macro to the TV driver so its (tiny) pin-base
    # path can key off the same compile flag other drivers use.
    target_compile_definitions(tv_driver PUBLIC PLATFORM_${BOARD_VARIANT}=1)
endif()

# I2S audio source is only compiled on platforms with an I2S DAC wired up.
# PWM-only boards (DV / PC / Z0) skip it entirely — the pins the driver
# references aren't defined by their board headers.
set(_AUDIO_I2S_SRC)
if(PLATFORM STREQUAL "m1" OR PLATFORM STREQUAL "m2")
    list(APPEND _AUDIO_I2S_SRC
        drivers/audio.c
        drivers/audio_i2s_opt.S
    )
endif()

add_library(drivers
    ${_VIDEO_DRIVER_SRC}
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/dlmalloc.c
    ${_AUDIO_I2S_SRC}
    drivers/pwm_audio/pwm_audio.c
    drivers/uart_logging.c
)

# dlmalloc compiled in mspace-only mode — gives us real malloc/free
# inside PSRAM while staying isolated from newlib's SRAM heap.
set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1")

target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
# Platform macro must be PUBLIC so HDMI.h (which is included from other
# libraries via the drivers target) picks up PLATFORM_Z0 / PLATFORM_M1
# when gating pin-ordering inside the header itself.
target_compile_definitions(drivers PUBLIC
    BOARD_${BOARD_VARIANT}
    PLATFORM_${BOARD_VARIANT}=1
)
target_compile_definitions(drivers PRIVATE
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
target_link_libraries(drivers
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_spi
    hardware_gpio
    hardware_clocks
    hardware_irq
    hardware_vreg
)

if(HDMI_HSTX)
    target_link_libraries(drivers pico_multicore pico_hdmi i2s_audio)
    target_compile_definitions(drivers PUBLIC HDMI_HSTX=1)
endif()

if(VIDEO_COMPOSITE)
    target_link_libraries(drivers pico_multicore tv_driver)
    target_compile_definitions(drivers PUBLIC VIDEO_COMPOSITE=1)
endif()

#-----------------------------------------------------------------------------
# fMSX core (portable C)
#-----------------------------------------------------------------------------
set(FMSX_CORE
    src/fMSX/MSX.c
    src/fMSX/V9938.c
    src/fMSX/Patch.c
    src/fMSX/I8251.c
    src/Z80/Z80.c
)

# Minimal EMULib — chip emulations + SHA1 + MCF + Sound (for InitSound/etc).
# Sound.c is compiled with NO_AUDIO_PLAYBACK so the host InitAudio/WriteAudio
# functions aren't required; our own thin wrappers live in src/Pico.
set(EMULIB_SRC
    src/EMULib/AY8910.c
    src/EMULib/YM2413.c
    src/EMULib/SCC.c
    src/EMULib/I8255.c
    src/EMULib/WD1793.c
    src/EMULib/FDIDisk.c
    src/EMULib/Floppy.c
    src/EMULib/SHA1.c
    src/EMULib/MCF.c
    src/EMULib/Sound.c
)

# Pico platform layer — replaces fMSX.c/Unix.c/Console.c/NetPlay.c/Touch/Image
set(PICO_PORT
    src/Pico/main.c
    src/Pico/platform.c
    src/Pico/msx_bios.c
    src/Pico/pico_fatfs_io.c
    src/Pico/ui_font.c
    src/Pico/ui_draw.c
    src/Pico/msx_loader.c
    src/Pico/msx_ui.c
    src/Pico/msx_settings.c
    src/Pico/msx_state.c
)

add_executable(frank-msx
    ${FMSX_CORE}
    ${EMULIB_SRC}
    ${PICO_PORT}
)

target_include_directories(frank-msx PRIVATE
    src
    src/fMSX
    src/Z80
    src/EMULib
    src/Pico
    drivers
    drivers/fatfs
    drivers/sdcard
    drivers/nespad
    drivers/ps2
)

if(USB_HID_ENABLED)
    target_include_directories(frank-msx PRIVATE drivers/usbhid)
endif()

if(HDMI_HSTX)
    target_compile_definitions(frank-msx PRIVATE HDMI_HSTX=1)
    target_include_directories(frank-msx PRIVATE drivers drivers/i2s)
    target_link_libraries(frank-msx i2s_audio)
endif()

if(VIDEO_COMPOSITE)
    target_compile_definitions(frank-msx PRIVATE VIDEO_COMPOSITE=1)
    target_link_libraries(frank-msx tv_driver)
endif()

target_compile_definitions(frank-msx PRIVATE
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    PICO_ON_DEVICE=1
    FRANK_MSX_VERSION="${FRANK_MSX_VERSION_STR}"
    # fMSX / EMULib configuration
    LSB_FIRST=1
    BPP8=1
    BPS16=1                 # 16-bit signed audio samples (for I2S path)
    NARROW=1                # skip WideScreen buffer (saves RAM)
    # AY(6) + SCC(5) + OPLL(9) = 20 sound channels. Default SND_CHANNELS
    # is 16 (= MIDI_CHANNELS), which would silently truncate OPLL.
    SND_CHANNELS=20
    MIDI_CHANNELS=20
    FRANK_MSX_PSRAM=1             # route all MSX chunk allocations to 8 MB PSRAM
    FRANK_MSX_MODEL=${MSX_MODEL}
    FRANK_MSX_SOFT_LIMITER=1      # 4:1 soft-knee limiter in place of hard int16 clamp
)

# Peripheral pins per platform — these get compiled into the standalone
# sdcard / nespad / ps2 drivers, which do NOT include board_config.h and
# therefore need the pins as -D flags at compile time.
#
# Pin values mirror murmnes/src/platform/pico/board_*.h so both projects
# stay in lockstep.
if(PLATFORM STREQUAL "m2")
    set(_SD_SCK 6)
    set(_SD_MOSI 7)
    set(_SD_MISO 4)
    set(_SD_CS 5)
    set(_NES_CLK 20)
    set(_NES_LATCH 21)
    set(_NES_DATA 26)
elseif(PLATFORM STREQUAL "m1")
    set(_SD_SCK 2)
    set(_SD_MOSI 3)
    set(_SD_MISO 4)
    set(_SD_CS 5)
    set(_NES_CLK 14)
    set(_NES_LATCH 15)
    set(_NES_DATA 16)
elseif(PLATFORM STREQUAL "dv")
    # SCK=5 can't map to hardware SPI — forced onto PIO-SPI below.
    set(_SD_SCK 5)
    set(_SD_MOSI 18)
    set(_SD_MISO 19)
    set(_SD_CS 22)
    set(_NES_CLK 14)
    set(_NES_LATCH 15)
    set(_NES_DATA 20)
elseif(PLATFORM STREQUAL "pc")
    set(_SD_SCK 6)
    set(_SD_MOSI 7)
    set(_SD_MISO 4)
    set(_SD_CS 22)
    set(_NES_CLK 5)
    set(_NES_LATCH 9)
    set(_NES_DATA 20)
elseif(PLATFORM STREQUAL "z0")
    # Pins above 29 can't use SPI0 — hardware SPI1 is required.
    set(_SD_SCK 30)
    set(_SD_MOSI 31)
    set(_SD_MISO 40)
    set(_SD_CS 43)
    set(_NES_CLK 4)
    set(_NES_LATCH 5)
    set(_NES_DATA 7)
endif()

target_compile_definitions(frank-msx PRIVATE
    SDCARD_PIN_SPI0_CS=${_SD_CS}
    SDCARD_PIN_SPI0_SCK=${_SD_SCK}
    SDCARD_PIN_SPI0_MOSI=${_SD_MOSI}
    SDCARD_PIN_SPI0_MISO=${_SD_MISO}
    NESPAD_GPIO_CLK=${_NES_CLK}
    NESPAD_GPIO_LATCH=${_NES_LATCH}
    NESPAD_GPIO_DATA=${_NES_DATA}
)

# Z0 wires SD card to hardware SPI1 (pins above 29 aren't on SPI0).
if(PLATFORM STREQUAL "z0")
    target_compile_definitions(frank-msx PRIVATE SDCARD_SPI_BUS=spi1)
endif()

# Pimoroni Pico DV Demo: SCK=5 can't map to a single HW SPI peripheral,
# so SD goes through PIO-SPI on pio1/SM3 (pio0 = HDMI, pio1 SM0..SM2
# already taken by audio/nespad/ps2 in init order).
if(PLATFORM STREQUAL "dv")
    target_compile_definitions(frank-msx PRIVATE
        SDCARD_PIO=pio1
        SDCARD_PIO_SM=3
    )
endif()

target_compile_options(frank-msx PRIVATE
    -O2
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-braces
    -Wno-implicit-fallthrough
    -Wno-pointer-sign
)

target_link_libraries(frank-msx
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_pwm
    hardware_irq
    hardware_vreg
    hardware_clocks
    sdcard
    fatfs
    drivers
    nespad
    ps2_driver
)

if(USB_HID_ENABLED)
    target_link_libraries(frank-msx usbhid)
endif()

# Dev builds (USB_HID off) use USB CDC stdio for printf; release (USB_HID on)
# disables it because USB HID needs exclusive use of the controller.
pico_enable_stdio_uart(frank-msx 0)
if(USB_HID_ENABLED)
    pico_enable_stdio_usb(frank-msx 0)
else()
    pico_enable_stdio_usb(frank-msx 1)
endif()

pico_add_extra_outputs(frank-msx)
