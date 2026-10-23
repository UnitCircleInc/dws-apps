/*
 * sensor_wasm.c — DWT Sensor Board Tester, WASM compute module
 *
 * Compiled with Emscripten (emcc).  Provides COBS framing, CBOR encode/decode,
 * and the FFT pipeline.  The JavaScript layer owns serial I/O and rendering.
 *
 * COBS is implemented inline here to avoid the ssize_t typedef in utils/cobs.h
 * conflicting with Emscripten's wasm32 definition (int vs intmax_t).
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <emscripten.h>

/* CBOR codec from utils/ (cbor.c compiled as a separate translation unit) */
#include "../utils/cbor.h"

/* kissfft (compiled as separate translation units) */
#include "kiss_fft.h"
#include "kiss_fftr.h"

/* -------------------------------------------------------------------------
   Inline COBS — avoids utils/cobs.h ssize_t conflict in wasm32
   ------------------------------------------------------------------------- */

#define COBS_ENC_SIZE(n) ((((n) + 253) / 254) + (n))

static int cobs_enc_w(uint8_t *out, const uint8_t *in, size_t n) {
    size_t r = 0, i = 0, cp = r++;
    uint8_t code = 1;
    while (i < n) {
        if (in[i] == 0) {
            out[cp] = code; cp = r++; code = 1;
        } else {
            out[r++] = in[i];
            if (++code == 0xFF) { out[cp] = 0xFF; cp = r++; code = 1; }
        }
        i++;
    }
    out[cp] = code;
    return (int)r;
}

static int cobs_dec_w(uint8_t *out, const uint8_t *in, size_t n) {
    size_t r = 0, i = 0;
    while (i < n) {
        uint8_t code = in[i++];
        if (code == 0) return -1;
        for (uint8_t j = 1; j < code; j++) {
            if (i >= n) return -1;
            out[r++] = in[i++];
        }
        if (code != 0xFF && i < n) out[r++] = 0;
    }
    return (int)r;
}

/* -------------------------------------------------------------------------
   Constants  (match main.cpp)
   ------------------------------------------------------------------------- */

#define N           1024
#define SAMPLE_RATE 3989.0f
#define PRES_WINDOW 100
#define ENABLE      7
#define POLL_MS     0

/* -------------------------------------------------------------------------
   Signal-processing state
   ------------------------------------------------------------------------- */

static float          g_blackman[N];
static float          g_blackman_sum;
static kiss_fftr_cfg  g_fft_cfg;

static float  g_h_sam[N * 2];
static int    g_h_sam_n;
static float  g_p_sam[PRES_WINDOW];

/* Output buffers — JS reads via pointer returned by accessor functions */
static float g_fft_mag[N / 2 + 1];
static float g_temp_c;
static char  g_status[64];

/* -------------------------------------------------------------------------
   Exported API
   ------------------------------------------------------------------------- */

EMSCRIPTEN_KEEPALIVE void sensor_init(void) {
    g_blackman_sum = 0.0f;
    for (int i = 0; i < N; i++) {
        float w = 0.42f
                - 0.5f  * cosf(2.0f * (float)M_PI * i / (N - 1))
                + 0.08f * cosf(4.0f * (float)M_PI * i / (N - 1));
        g_blackman[i]   = w;
        g_blackman_sum += w;
    }
    g_fft_cfg = kiss_fftr_alloc(N, 0, NULL, NULL);
    g_h_sam_n = 0;
    g_temp_c  = 0.0f;
    memset(g_fft_mag, 0, sizeof(g_fft_mag));
    memset(g_p_sam,   0, sizeof(g_p_sam));
    memset(g_status,  0, sizeof(g_status));
}

/* COBS-decode a raw frame (bytes between null delimiters, nulls excluded).
   Returns decoded byte count, or -1 on error. */
EMSCRIPTEN_KEEPALIVE int sensor_cobs_dec(const uint8_t *in, int in_len,
                                         uint8_t *out, int /*out_cap*/) {
    return cobs_dec_w(out, in, (size_t)in_len);
}

/* ---- Command encoders — write a complete COBS-framed CBOR command ---- */

static int encode_cmd_1(const char *cmd_name, uint8_t *buf, int buf_size) {
    uint8_t cbor[64];
    cbor_stream_t cs;
    cbor_init(&cs, cbor, sizeof(cbor));
    cbor_write_map(&cs, 1);
    cbor_write_text(&cs, "cmd");
    cbor_write_text(&cs, cmd_name);
    if (cbor_error(&cs) != CBOR_ERROR_NONE) return -1;
    int clen = (int)cbor_read_avail(&cs);

    uint8_t enc[2 + COBS_ENC_SIZE(64)];
    enc[0] = 0;
    int elen = cobs_enc_w(enc + 1, cbor, (size_t)clen);
    enc[1 + elen] = 0;
    int total = 2 + elen;
    if (total > buf_size) return -1;
    memcpy(buf, enc, (size_t)total);
    return total;
}

EMSCRIPTEN_KEEPALIVE int sensor_encode_info(uint8_t *buf, int buf_size) {
    return encode_cmd_1("info", buf, buf_size);
}

EMSCRIPTEN_KEEPALIVE int sensor_encode_bl_done(uint8_t *buf, int buf_size) {
    return encode_cmd_1("bl-done", buf, buf_size);
}

EMSCRIPTEN_KEEPALIVE int sensor_encode_poll(uint8_t *buf, int buf_size) {
    uint8_t cbor[128];
    cbor_stream_t cs;
    cbor_init(&cs, cbor, sizeof(cbor));
    cbor_write_map(&cs, 3);
    cbor_write_text(&cs, "cmd");    cbor_write_text(&cs, "poll");
    cbor_write_text(&cs, "enable"); cbor_write_uint64(&cs, ENABLE);
    cbor_write_text(&cs, "next");   cbor_write_uint64(&cs, POLL_MS);
    if (cbor_error(&cs) != CBOR_ERROR_NONE) return -1;
    int clen = (int)cbor_read_avail(&cs);

    uint8_t enc[2 + COBS_ENC_SIZE(128)];
    enc[0] = 0;
    int elen = cobs_enc_w(enc + 1, cbor, (size_t)clen);
    enc[1 + elen] = 0;
    int total = 2 + elen;
    if (total > buf_size) return -1;
    memcpy(buf, enc, (size_t)total);
    return total;
}

/* Decode a CBOR info response.
   Returns 1 if bl-ver or app-ver found; writes "BL:<x> APP:<y>" into ver_out. */
EMSCRIPTEN_KEEPALIVE int sensor_decode_info(const uint8_t *data, int len,
                                            char *ver_out, int ver_size) {
    char bl[64] = "", app[64] = "";
    size_t bl_len = sizeof(bl), ap_len = sizeof(app);
    bool bl_ok = false, ap_ok = false;

    cbor_stream_t rs;
    cbor_init(&rs, (uint8_t *)data, (size_t)len);
    cbor_unpack(&rs, "{.bl-ver:?s,.app-ver:?s}",
                &bl_ok, bl, &bl_len,
                &ap_ok, app, &ap_len);

    if (!bl_ok && !ap_ok) return 0;
    snprintf(ver_out, (size_t)ver_size, "BL: %s  APP: %s", bl, app);
    return 1;
}

/* Process a decoded CBOR poll response.
   Returns bitmask: 1=FFT updated, 2=pressure updated, 4=temp updated.
   Returns -1 on CBOR parse error. */
EMSCRIPTEN_KEEPALIVE int sensor_process_poll(const uint8_t *data, int len) {
    uint8_t h_buf[N * 4];
    uint8_t p_buf[PRES_WINDOW * 4];
    uint8_t t_buf[16];
    char    s_val[64];
    size_t  h_n = sizeof(h_buf), p_n = sizeof(p_buf);
    size_t  t_n = sizeof(t_buf), s_len = sizeof(s_val);
    bool h_ok = false, p_ok = false, t_ok = false;
    bool m_ok = false, s_ok = false, m = false;

    cbor_stream_t rs;
    cbor_init(&rs, (uint8_t *)data, (size_t)len);
    cbor_error_t ce = cbor_unpack(&rs,
        "{.h:?b,.p:?b,.t:?b,.m:??,.s:?s}",
        &h_ok, h_buf, &h_n,
        &p_ok, p_buf, &p_n,
        &t_ok, t_buf, &t_n,
        &m_ok, &m,
        &s_ok, s_val, &s_len);

    if (ce != CBOR_ERROR_NONE) return -1;

    int result = 0;

    /* Hydrophone accumulation */
    if (h_ok) {
        int h_count = (int)(h_n / 4);
        int space   = N * 2 - g_h_sam_n;
        int copy    = h_count < space ? h_count : space;
        if (copy > 0) {
            memcpy(g_h_sam + g_h_sam_n, h_buf, (size_t)copy * 4);
            g_h_sam_n += copy;
        }
        if (g_h_sam_n >= N) {
            /* DC removal */
            float mean = 0.0f;
            for (int i = 0; i < N; i++) mean += g_h_sam[i];
            mean /= N;

            /* Blackman window */
            float windowed[N];
            for (int i = 0; i < N; i++)
                windowed[i] = (g_h_sam[i] - mean) * g_blackman[i];

            /* FFT → power → dB */
            kiss_fft_cpx spectrum[N / 2 + 1];
            kiss_fftr(g_fft_cfg, windowed, spectrum);

            float F2 = g_blackman_sum * g_blackman_sum;
            for (int k = 0; k <= N / 2; k++) {
                float power = (spectrum[k].r * spectrum[k].r +
                               spectrum[k].i * spectrum[k].i) / F2;
                if (k > 0 && k < N / 2) power *= 2.0f;
                g_fft_mag[k] = 10.0f * log10f(power + 1e-30f);
            }
            g_h_sam_n = 0;
            result |= 1;
        }
    }

    /* Pressure rolling window */
    if (p_ok) {
        int p_count = (int)(p_n / 4);
        if (p_count > 0) {
            float p_new[PRES_WINDOW];
            int copy = p_count < PRES_WINDOW ? p_count : PRES_WINDOW;
            memcpy(p_new, p_buf + (size_t)(p_count - copy) * 4,
                   (size_t)copy * 4);
            if (copy >= PRES_WINDOW) {
                memcpy(g_p_sam, p_new, sizeof(g_p_sam));
            } else {
                memmove(g_p_sam, g_p_sam + copy,
                        (size_t)(PRES_WINDOW - copy) * sizeof(float));
                memcpy(g_p_sam + (PRES_WINDOW - copy), p_new,
                       (size_t)copy * sizeof(float));
            }
            for (int i = 0; i < PRES_WINDOW; i++) {
                if (g_p_sam[i] < -100.0f)  g_p_sam[i] = -100.0f;
                if (g_p_sam[i] > 2500.0f)  g_p_sam[i] =  2500.0f;
            }
            result |= 2;
        }
    }

    /* Temperature */
    if (t_ok && t_n >= 4) {
        memcpy(&g_temp_c, t_buf, 4);
        result |= 4;
    }

    /* Status string */
    if (s_ok) {
        strncpy(g_status, s_val, sizeof(g_status) - 1);
        g_status[sizeof(g_status) - 1] = '\0';
    }

    return result;
}

/* Accessors — return pointers into WASM linear memory so JS can view the
   arrays directly via Float32Array without copying. */
EMSCRIPTEN_KEEPALIVE float *sensor_fft_mag(void)      { return g_fft_mag; }
EMSCRIPTEN_KEEPALIVE float *sensor_pres(void)          { return g_p_sam;   }
EMSCRIPTEN_KEEPALIVE float *sensor_temp(void)          { return &g_temp_c; }
EMSCRIPTEN_KEEPALIVE char  *sensor_status(void)        { return g_status;  }
EMSCRIPTEN_KEEPALIVE int    sensor_fft_len(void)       { return N / 2 + 1; }
EMSCRIPTEN_KEEPALIVE int    sensor_pres_len(void)      { return PRES_WINDOW; }
EMSCRIPTEN_KEEPALIVE float  sensor_sample_rate(void)   { return SAMPLE_RATE; }
