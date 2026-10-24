/*
 * sensor_wasm.c — DWS Sensor Board Tester, WASM compute module
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

/* -------------------------------------------------------------------------
   Simulator board — stands in for a sensor board on the serial link.  JS
   feeds it each COBS frame the app writes; it answers like the firmware:
   "info" -> bootloader/app versions, "bl-done" -> start the app, "poll" ->
   a poll response covering the simulated time since the previous poll.
   ------------------------------------------------------------------------- */

#define SIM_PRES_RATE 100.0f    /* PRES_WINDOW samples per second */
#define SIM_VERSION   "sim"

static bool     g_sim_app_running;
static double   g_sim_last_poll_ms;
static double   g_sim_h_t;      /* hydrophone sample clock [s] */
static double   g_sim_p_t;      /* pressure sample clock [s] */
static uint32_t g_sim_rng = 0x2545F491u;

/* Approximately normal noise (Irwin–Hall, 4 uniforms), unit variance. */
static float sim_noise(void) {
    float s = 0.0f;
    for (int i = 0; i < 4; i++) {
        g_sim_rng ^= g_sim_rng << 13;
        g_sim_rng ^= g_sim_rng >> 17;
        g_sim_rng ^= g_sim_rng << 5;
        s += (float)g_sim_rng / 4294967296.0f;
    }
    return (s - 2.0f) * 1.7320508f;
}

/* Write a CBOR poll response covering dt_ms of simulated time into cs. */
static void sim_poll_response(cbor_stream_t *cs, double dt_ms) {
    float h[N];
    float p[PRES_WINDOW];
    int h_n = (int)(SAMPLE_RATE * dt_ms / 1000.0 + 0.5);
    int p_n = (int)(SIM_PRES_RATE * dt_ms / 1000.0 + 0.5);
    if (h_n > N)           h_n = N;
    if (p_n > PRES_WINDOW) p_n = PRES_WINDOW;

    /* Hydrophone [Pa]: 750 Hz test tone with a slow level wobble, its 2nd
       harmonic, 50 Hz pickup, and a broadband noise floor. */
    for (int i = 0; i < h_n; i++) {
        double t = g_sim_h_t + i / (double)SAMPLE_RATE;
        float  a = 2.0f + 0.5f * sinf(2.0f * (float)M_PI * 0.2f * (float)t);
        h[i] = a     * sinf((float)fmod(2.0 * M_PI *  750.0 * t, 2.0 * M_PI))
             + 0.05f * sinf((float)fmod(2.0 * M_PI * 1500.0 * t, 2.0 * M_PI))
             + 0.02f * sinf((float)fmod(2.0 * M_PI *   50.0 * t, 2.0 * M_PI))
             + 0.002f * sim_noise();
    }
    g_sim_h_t += h_n / (double)SAMPLE_RATE;

    /* Pressure [kPa]: atmospheric baseline with a press pulse every 1.25 s. */
    for (int i = 0; i < p_n; i++) {
        double t     = g_sim_p_t + i / (double)SIM_PRES_RATE;
        double phase = fmod(t, 1.25) / 0.6;
        float  press = phase < 1.0 ? (float)(0.5 - 0.5 * cos(2.0 * M_PI * phase))
                                   : 0.0f;
        p[i] = 101.3f + 1800.0f * press + 1.5f * sim_noise();
    }
    g_sim_p_t += p_n / (double)SIM_PRES_RATE;

    float temp = 21.5f + 0.3f * sinf((float)fmod(2.0 * M_PI * g_sim_p_t / 60.0, 2.0 * M_PI));

    cbor_write_map(cs, 4);
    cbor_write_text(cs, "h"); cbor_write_bytes(cs, (const uint8_t *)h, (size_t)h_n * 4);
    cbor_write_text(cs, "p"); cbor_write_bytes(cs, (const uint8_t *)p, (size_t)p_n * 4);
    cbor_write_text(cs, "t"); cbor_write_bytes(cs, (const uint8_t *)&temp, 4);
    cbor_write_text(cs, "s"); cbor_write_text(cs, "sim");
}

/* Power-on reset: the simulated board starts in its bootloader. */
EMSCRIPTEN_KEEPALIVE void sensor_sim_reset(void) {
    g_sim_app_running  = false;
    g_sim_last_poll_ms = -1.0;
}

/* Handle one frame received by the simulated board (bytes between null
   delimiters).  now_ms is the host clock, used to size poll responses.
   Writes a complete null-delimited COBS-framed reply into out and returns
   its length; returns 0 when there is no reply, -1 on error. */
EMSCRIPTEN_KEEPALIVE int sensor_sim_rx(const uint8_t *in, int in_len,
                                       uint8_t *out, int out_cap, double now_ms) {
    static uint8_t dec[256];
    static uint8_t cbor[8192];

    if (in_len <= 0 || in_len > (int)sizeof(dec)) return -1;
    int dec_len = cobs_dec_w(dec, in, (size_t)in_len);
    if (dec_len <= 0) return -1;

    char     cmd[16] = "";
    size_t   cmd_len = sizeof(cmd);
    unsigned enable = 0, next = 0;
    bool     cmd_ok = false, en_ok = false, nx_ok = false;
    cbor_stream_t rs;
    cbor_init(&rs, dec, (size_t)dec_len);
    if (cbor_unpack(&rs, "{.cmd:?s,.enable:?I,.next:?I}",
                    &cmd_ok, cmd, &cmd_len, &en_ok, &enable, &nx_ok, &next)
            != CBOR_ERROR_NONE || !cmd_ok)
        return -1;

    cbor_stream_t cs;
    cbor_init(&cs, cbor, sizeof(cbor));

    if (strcmp(cmd, "info") == 0) {
        cbor_write_map(&cs, 2);
        cbor_write_text(&cs, "bl-ver");  cbor_write_text(&cs, SIM_VERSION);
        cbor_write_text(&cs, "app-ver"); cbor_write_text(&cs, SIM_VERSION);
    } else if (strcmp(cmd, "bl-done") == 0) {
        g_sim_app_running = true;
        return 0;
    } else if (strcmp(cmd, "poll") == 0 && g_sim_app_running) {
        double dt = g_sim_last_poll_ms < 0 ? 20.0 : now_ms - g_sim_last_poll_ms;
        if (dt < 0)    dt = 0;
        if (dt > 250)  dt = 250;    /* after a stall, don't overflow the frame */
        g_sim_last_poll_ms = now_ms;
        sim_poll_response(&cs, dt);
    } else {
        return 0;                   /* unknown, or poll while in bootloader */
    }

    if (cbor_error(&cs) != CBOR_ERROR_NONE) return -1;
    int clen = (int)cbor_read_avail(&cs);
    if (2 + COBS_ENC_SIZE(clen) > out_cap) return -1;
    out[0] = 0;
    int elen = cobs_enc_w(out + 1, cbor, (size_t)clen);
    out[1 + elen] = 0;
    return 2 + elen;
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
