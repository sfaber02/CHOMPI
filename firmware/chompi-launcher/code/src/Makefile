# CHOMPI firmware launcher
#
# Build with ARM's official GCC 10.3-2021.10, the same toolchain the stock
# firmwares use:
#
#   PATH=/path/to/gcc-arm-none-eabi-10.3-2021.10/bin:$PATH make -j8
#
# Output is build/CHOMPI.bin, which goes in the ROOT of the SD card. The
# firmwares it launches go in /FIRMWARE on the same card.

TARGET = CHOMPI

DEBUG=0
OPT=-O3

APP_TYPE=BOOT_SRAM
LDSCRIPT=chompi_sram.lds

CPP_SOURCES = launcher_main.cpp \
	encoder.cpp

USE_FATFS = 1

# Shared with the upstream firmwares rather than duplicated: these ship
# prebuilt, and rebuilding them against a different compiler is what causes the
# SD-card trouble CHOMPI Club warns about.
CHOMPI_LIBS ?= ../../../chompi-wave/code/libs

LIBDAISY_DIR = $(CHOMPI_LIBS)/libDaisy
DAISYSP_DIR = $(CHOMPI_LIBS)/DaisySP

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
