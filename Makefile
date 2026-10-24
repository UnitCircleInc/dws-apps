# =============================================================================
# DWS Sensor Board Tester — WebAssembly build
#
# Prerequisites:
#   Emscripten (emcc):  brew install emscripten
#                       (or https://emscripten.org/docs/getting_started)
#   python3, git
#
# One-time vendor setup:
#   make setup
#
# Build the WASM module (web/sensor-board-tester/app/sensor.js):
#   make web
#
# Serve the site locally on http://localhost:8080:
#   make run
#
# Build the deployable site (hub, tool pages, apps, standalone downloads) in _site/:
#   make site
#
# Regenerate the landing-page screenshots (needs Google Chrome):
#   make screenshots
#
# Clean:
#   make clean
# =============================================================================

# ---------- Vendor paths ------------------------------------------------------
VENDOR      := vendor
KISSFFT     := $(VENDOR)/kissfft
KISSFFT_URL := https://github.com/mborgerding/kissfft
KISSFFT_REF := 5399a98411c57b3da734d0050744e14103f898ab

APP  := web/sensor-board-tester/app
SITE := _site

# ---------- Top-level targets -------------------------------------------------
.PHONY: web setup clean run site screenshots

setup:
	@test -d $(KISSFFT) || ( \
		mkdir -p $(KISSFFT) && cd $(KISSFFT) && \
		git init -q && \
		git fetch -q --depth=1 $(KISSFFT_URL) $(KISSFFT_REF) && \
		git checkout -q FETCH_HEAD )

clean:
	rm -rf $(APP)/sensor.js $(SITE)

# ---------- WebAssembly (Emscripten) ------------------------------------------
# Output: $(APP)/sensor.js with the wasm embedded (SINGLE_FILE), so the app
# also works when inlined into one HTML file and opened from file://.

$(APP)/sensor.js: sensor_wasm.c utils/cbor.c utils/cbor.h utils/log.h \
               $(KISSFFT)/kiss_fft.c $(KISSFFT)/kiss_fftr.c
	emcc sensor_wasm.c utils/cbor.c \
		$(KISSFFT)/kiss_fft.c $(KISSFFT)/kiss_fftr.c \
		-I. -Iutils -I$(KISSFFT) \
		-D_Float16=uint16_t \
		-O2 \
		-s EXPORTED_FUNCTIONS="['_sensor_init','_sensor_cobs_dec','_sensor_encode_info','_sensor_encode_bl_done','_sensor_encode_poll','_sensor_decode_info','_sensor_process_poll','_sensor_fft_mag','_sensor_pres','_sensor_temp','_sensor_status','_sensor_fft_len','_sensor_pres_len','_sensor_sample_rate','_sensor_sim_reset','_sensor_sim_rx','_malloc','_free']" \
		-s EXPORTED_RUNTIME_METHODS="['HEAPU8','HEAPF32']" \
		-s MODULARIZE=1 \
		-s EXPORT_NAME="SensorModule" \
		-s SINGLE_FILE=1 \
		-o $@

web: $(APP)/sensor.js

run: web
	python3 -m http.server -d web 8080

# ---------- Site --------------------------------------------------------------

site: web
	python3 tools/build_site.py --src web --out $(SITE)

screenshots: web
	python3 tools/screenshots.py --app $(APP)/index.html --out web/sensor-board-tester/img
