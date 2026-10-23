# =============================================================================
# DWT Sensor Board Tester — C/C++ build
#
# Prerequisites (install before building):
#   macOS:   brew install glfw
#   Linux:   apt install libglfw3-dev
#   Windows: MSYS2/MinGW-w64 → pacman -S mingw-w64-x86_64-glfw
#
# One-time vendor setup:
#   make setup
#
# Build:
#   make
#
# Clean:
#   make clean
# =============================================================================

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
  PLATFORM := macos
else ifeq ($(UNAME_S),Linux)
  PLATFORM := linux
else
  PLATFORM := windows
endif

# ---------- Compiler ----------------------------------------------------------
CXX    := g++
CC     := gcc
CSTD   := -std=c11
CXXSTD := -std=c++17
OPT    := -O2
WARN   := -Wall -Wextra -Wno-unused-parameter

# ---------- Vendor paths ------------------------------------------------------
VENDOR  := vendor
KISSFFT := $(VENDOR)/kissfft


# ---------- Top-level targets -------------------------------------------------
.PHONY: web setup clean run

setup:
	mkdir -p $(VENDOR)
	@test -d $(KISSFFT) || git clone --depth=1 https://github.com/mborgerding/kissfft $(KISSFFT)

clean:
	rm sensor.js sensor.wasm

# ---------- WebAssembly (Emscripten) ------------------------------------------
# Prerequisites: brew install emscripten  (or https://emscripten.org/docs/getting_started)
# Output: sensor.js + sensor.wasm — served alongside index.html and app.js.
#

sensor.js: sensor_wasm.c utils/cbor.c utils/cbor.h utils/log.h \
               $(KISSFFT)/kiss_fft.c $(KISSFFT)/kiss_fftr.c
	emcc sensor_wasm.c utils/cbor.c \
		$(KISSFFT)/kiss_fft.c $(KISSFFT)/kiss_fftr.c \
		-I. -Iutils -I$(KISSFFT) \
		-D_Float16=uint16_t \
		-O2 \
		-s EXPORTED_FUNCTIONS="['_sensor_init','_sensor_cobs_dec','_sensor_encode_info','_sensor_encode_bl_done','_sensor_encode_poll','_sensor_decode_info','_sensor_process_poll','_sensor_fft_mag','_sensor_pres','_sensor_temp','_sensor_status','_sensor_fft_len','_sensor_pres_len','_sensor_sample_rate','_malloc','_free']" \
		-s EXPORTED_RUNTIME_METHODS="['HEAPU8','HEAPF32']" \
		-s MODULARIZE=1 \
		-s EXPORT_NAME="SensorModule" \
		-o sensor.js

web: sensor.js

run: sensor.js
	# Serve locally with:
	python3 -m http.server -d . 8080

