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

# Video + audio + PSRAM live in a single "drivers" library
add_library(drivers
    drivers/HDMI.c
    drivers/hdmi_scanline.S
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/audio.c
    drivers/audio_i2s_opt.S
    drivers/uart_logging.c
)

target_include_directories(drivers PUBLIC drivers src)
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
pico_generate_pio_header(drivers ${CMAKE_CURRENT_LIST_DIR}/drivers/audio_i2s.pio)
target_link_libraries(drivers
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_spi
    hardware_gpio
    hardware_clocks
    hardware_irq
    hardware_vreg
)

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
    NARROW=1                # skip WideScreen buffer (saves RAM)
    FRANK_MSX_NO_OPTIONAL_ROMS=1  # skip CMOS/KANJI/RS232/MSXDOS2/PAINTER/FMPAC/GMASTER probe
    FRANK_MSX_PSRAM=1             # route all MSX chunk allocations to 8 MB PSRAM
    FRANK_MSX_MODEL=${MSX_MODEL}
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
