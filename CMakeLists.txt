cmake_minimum_required(VERSION 3.13)

# Target: Raspberry Pi Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board variant — only M2 is supported right now.
set(BOARD_VARIANT "M2" CACHE STRING "Board variant (M2 only for now)")

# CPU / PSRAM / FLASH clocking (MHz). Defaults chosen for reliability.
set(CPU_SPEED   "252" CACHE STRING "CPU clock in MHz")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz")
set(FLASH_SPEED "66"  CACHE STRING "Flash target max frequency in MHz")

# MSX model: 1 = MSX1, 2 = MSX2, 3 = MSX2+
set(MSX_MODEL "3" CACHE STRING "MSX model to emulate (1, 2, or 3)")

# Optional USB HID host (disabled by default since we focus on PS/2)
option(USB_HID_ENABLED "Enable USB HID host" OFF)

# Video output driver selection — PIO HDMI by default; HSTX HDMI when
# HDMI_HSTX=ON; composite TV when VIDEO_COMPOSITE=ON (all three are
# mutually exclusive). HSTX carries HDMI audio as data-island packets;
# VIDEO_COMPOSITE generates a full PAL/NTSC software composite signal
# on the 8-bit HDMI connector (via an HDMI-to-RCA adapter or resistor
# DAC wired to GPIO 12..19).
option(HDMI_HSTX       "Use the RP2350 HSTX-based HDMI driver (with HDMI audio)" OFF)
option(VIDEO_COMPOSITE "Use the software composite-TV driver"                    OFF)

if(HDMI_HSTX AND VIDEO_COMPOSITE)
    message(FATAL_ERROR "HDMI_HSTX and VIDEO_COMPOSITE are mutually exclusive")
endif()

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 400)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
elseif(CPU_SPEED GREATER_EQUAL 300)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_55")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "Board: ${BOARD_VARIANT}  CPU: ${CPU_SPEED} MHz  PSRAM: ${PSRAM_SPEED} MHz  Flash: ${FLASH_SPEED} MHz  Voltage: ${CPU_VOLTAGE}")

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
    target_compile_options(tv_driver PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tv/tv_rename.h
        -Wno-error
        -Wno-incompatible-pointer-types
        -Wno-all
    )
    target_link_libraries(tv_driver
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
        hardware_clocks
    )
    if(BOARD_VARIANT STREQUAL "M2")
        # 8-bit DAC lives on the HDMI connector (GPIO 12..19).
        target_compile_definitions(tv_driver PUBLIC TV_BASE_PIN=12)
    endif()
endif()

add_library(drivers
    ${_VIDEO_DRIVER_SRC}
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/dlmalloc.c
    drivers/audio.c
    drivers/audio_i2s_opt.S
    drivers/pwm_audio/pwm_audio.c
    drivers/uart_logging.c
)

# dlmalloc compiled in mspace-only mode — gives us real malloc/free
# inside PSRAM while staying isolated from newlib's SRAM heap.
set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1")

target_include_directories(drivers PUBLIC drivers drivers/pwm_audio src)
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
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

# Peripheral pins per board variant (must match driver expectations).
# Pin values copied verbatim from murmsnes CMakeLists to stay in lockstep.
if(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(frank-msx PRIVATE
        SDCARD_PIN_SPI0_CS=5
        SDCARD_PIN_SPI0_SCK=6
        SDCARD_PIN_SPI0_MOSI=7
        SDCARD_PIN_SPI0_MISO=4
        NESPAD_GPIO_CLK=20
        NESPAD_GPIO_LATCH=21
        NESPAD_GPIO_DATA=26
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
