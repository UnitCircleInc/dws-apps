/**
 * app.js — DWT Sensor Board Tester, web front-end
 *
 * Requires Chrome 89+ or Edge 89+.
 * Web Serial API is NOT supported in Safari (as of 2025).
 */

/* =========================================================================
   WASM module bootstrap
   ========================================================================= */

let M;   // Emscripten module instance

async function loadWasm() {
    M = await SensorModule();
    M._sensor_init();
}

/* =========================================================================
   Plot rendering
   ========================================================================= */

const PAD = { top: 12, right: 18, bottom: 38, left: 58 };

function plotDims(canvas) {
    return {
        W: canvas.width,
        H: canvas.height,
        pw: canvas.width  - PAD.left - PAD.right,
        ph: canvas.height - PAD.top  - PAD.bottom,
    };
}

function drawPlot(ctx, data, xMin, xMax, yMin, yMax,
                  lineColor, xLabel, yLabel, title) {
    const { W, H, pw, ph } = plotDims(ctx.canvas);

    ctx.fillStyle = '#101020';
    ctx.fillRect(0, 0, W, H);

    /* Grid */
    const yTicks = [yMin, yMin*0.5, 0, yMax*0.5, yMax].filter(
        v => v >= yMin && v <= yMax);
    const xTicks = 5;

    ctx.strokeStyle = '#252540';
    ctx.lineWidth   = 1;
    ctx.font        = '10px monospace';
    ctx.fillStyle   = '#666';

    for (const yv of yTicks) {
        const y = PAD.top + ph * (1 - (yv - yMin) / (yMax - yMin));
        ctx.beginPath(); ctx.moveTo(PAD.left, y); ctx.lineTo(PAD.left + pw, y);
        ctx.stroke();
        ctx.textAlign = 'right';
        ctx.fillText(yv.toFixed(0), PAD.left - 4, y + 3);
    }

    for (let i = 0; i <= xTicks; i++) {
        const x  = PAD.left + pw * i / xTicks;
        const xv = xMin + (xMax - xMin) * i / xTicks;
        ctx.beginPath(); ctx.moveTo(x, PAD.top); ctx.lineTo(x, PAD.top + ph);
        ctx.stroke();
        ctx.textAlign = 'center';
        ctx.fillText(xv.toFixed(0), x, PAD.top + ph + 13);
    }

    /* Axis labels */
    ctx.fillStyle = '#999';
    ctx.font      = '11px monospace';
    ctx.textAlign = 'center';
    ctx.fillText(xLabel, PAD.left + pw / 2, H - 4);

    ctx.save();
    ctx.translate(13, PAD.top + ph / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText(yLabel, 0, 0);
    ctx.restore();

    /* Title */
    ctx.fillStyle   = '#ccc';
    ctx.font        = '11px monospace';
    ctx.textAlign   = 'left';
    ctx.fillText(title, PAD.left + 4, PAD.top - 1);

    /* Border */
    ctx.strokeStyle = '#444';
    ctx.lineWidth   = 1;
    ctx.strokeRect(PAD.left, PAD.top, pw, ph);

    if (!data || data.length === 0) return;

    /* Data line */
    ctx.save();
    ctx.beginPath();
    ctx.rect(PAD.left, PAD.top, pw, ph);
    ctx.clip();

    ctx.strokeStyle = lineColor;
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    for (let i = 0; i < data.length; i++) {
        const x = PAD.left + pw * i / (data.length - 1);
        const y = PAD.top  + ph * (1 - (data[i] - yMin) / (yMax - yMin));
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.restore();
}

/* =========================================================================
   Application state
   ========================================================================= */

const state = {
    port:       null,
    writer:     null,
    proto:      'idle',   // idle | info | bl_done | wait_app | polling
    /* Pre-allocated output buffers — filled in place by .set() to avoid
       creating new Float32Arrays on every poll and triggering GC. */
    fftData:    new Float32Array(513),   // N/2+1 for N=1024
    presData:   new Float32Array(100),   // PRES_WINDOW
    fftLabel:   'Hydrophone Response',
    presLabel:  'Pressure Response',
    tempStr:    '',
    statusStr:  'Disconnected — use Chrome or Edge',
    versionStr: '',
    pollCmd:    null,     // encoded poll frame, set when entering polling state
    pollCount:  0,
    parseErrors:0,
    rateFilt:   0,
    lastRateTs: 0,
    lastPollAt: 0,        // performance.now() of most recent poll sent (watchdog)
};

/* =========================================================================
   Serial / protocol
   ========================================================================= */

/* WASM scratch buffer pointers — allocated once after module load.
   We intentionally do NOT cache typed-array views (subarray) here because
   ALLOW_MEMORY_GROWTH can replace the underlying ArrayBuffer, detaching any
   cached views and causing a TypeError on the next .set() call.
   Always read M.HEAPU8 / M.HEAPF32 fresh at the call site instead. */
let cmdBufPtr, frameBufPtr, decodeBufPtr, verBufPtr;

const CMD_CAP   = 256;
const FRAME_CAP = 65536;

function allocateWasmBuffers() {
    cmdBufPtr    = M._malloc(CMD_CAP);
    frameBufPtr  = M._malloc(FRAME_CAP);
    decodeBufPtr = M._malloc(FRAME_CAP);
    verBufPtr    = M._malloc(256);
}

/* Singleton TextDecoder — reused on every call to avoid allocating a new
   ICU converter object per poll response (was creating 100+/s at fast rates). */
const _td = new TextDecoder();

/* Read a null-terminated UTF-8 string from WASM linear memory.
   Uses HEAPU8.slice() (copy) not subarray() (view): Chrome 111+ rejects
   decode() on resizable ArrayBuffers. */
function wasmStr(ptr) {
    if (!ptr) return '';
    let end = ptr;
    while (M.HEAPU8[end]) end++;
    return _td.decode(M.HEAPU8.slice(ptr, end));
}

function encodeCmd(fn) {
    const len = fn(cmdBufPtr, CMD_CAP);
    if (len <= 0) return null;
    return M.HEAPU8.slice(cmdBufPtr, cmdBufPtr + len);
}

function bufferToHex(ptr, length) {
  // Grab a subarray of the Emscripten/Wasm heap starting at the pointer
  const bytes = new Uint8Array(M.HEAPU8.buffer, ptr, length);

  // Convert each byte to a 2-character hex string and join them
  return Array.from(bytes)
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
}

function processDecodedFrame(decLen) {
    if (decLen <= 0) return;

    if (state.proto === 'info') {
        const ok = M._sensor_decode_info(decodeBufPtr, decLen, verBufPtr, 256);
        if (ok) {
            state.versionStr = wasmStr(verBufPtr);
            state.proto      = 'bl_done';
            console.log('[proto] info response received:', state.versionStr);
        }
        else {
            console.warn('[proto] info response failed to decode', bufferToHex(decodeBufPtr, decLen));
        }
        return;
    }

    if (state.proto === 'polling') {
        const res = M._sensor_process_poll(decodeBufPtr, decLen);
        if (res < 0) {
            state.parseErrors++;
            /* Keep the request-reply chain alive even on parse errors. */
            if (state.pollCmd && state.writer) {
                state.lastPollAt = performance.now();
                state.writer.write(state.pollCmd)
                    .catch(e => console.warn('[proto] poll write error:', e));
            }
            return;
        }

        state.pollCount++;
        const now = performance.now();
        const dt  = (now - state.lastRateTs) / 1000;
        if (dt >= 0.5) {
            state.rateFilt  = state.pollCount / dt;
            state.pollCount = 0;
            state.lastRateTs = now;
        }

        if (res & 1) {
            const ptr = M._sensor_fft_mag();
            const n   = M._sensor_fft_len();
            const sr  = M._sensor_sample_rate();
            /* Copy from WASM heap into the pre-allocated buffer without
               allocating a new Float32Array. subarray() is a zero-copy view
               used only transiently as the .set() source argument. */
            state.fftData.set(M.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + n));

            let peak = -Infinity, peakIdx = 0;
            for (let i = 0; i < n; i++) {
                if (state.fftData[i] > peak) { peak = state.fftData[i]; peakIdx = i; }
            }
            const peakHz = (peakIdx / 1024) * sr;
            state.fftLabel =
                `Hydrophone — peak: ${peak.toFixed(1)} dB @ ${peakHz.toFixed(0)} Hz`;
        }

        if (res & 2) {
            const ptr = M._sensor_pres();
            const n   = M._sensor_pres_len();
            state.presData.set(M.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + n));

            let pmin = Infinity, pmax = -Infinity;
            for (const v of state.presData) {
                if (v < pmin) pmin = v;
                if (v > pmax) pmax = v;
            }
            state.presLabel =
                `Pressure — min: ${pmin.toFixed(3)} kPa  max: ${pmax.toFixed(3)} kPa`;
        }

        if (res & 4) {
            const tempPtr = M._sensor_temp();
            const temp    = M.HEAPF32[tempPtr >> 2];
            const statPtr = M._sensor_status();
            const statStr = wasmStr(statPtr);
            state.tempStr = `${temp.toFixed(2)} °C [${statStr}]`;
        }

        /* Request-reply: send next poll immediately after each response.
           This eliminates setTimeout jitter — the poll interval tracks the
           actual serial round-trip rather than Chrome's scheduler. */
        if (state.pollCmd && state.writer) {
            state.lastPollAt = performance.now();
            state.writer.write(state.pollCmd)
                .catch(e => console.warn('[proto] poll write error:', e));
        }
    }
}

/* Frame extractor — accumulates bytes and calls processDecodedFrame on each
   null-delimited COBS frame. */
const rx = {
    buf:     new Uint8Array(65536),
    len:     0,
    inFrame: false,
    frameLen: 0,
};

function feedBytes(chunk) {
    for (let i = 0; i < chunk.length; i++) {
        const b = chunk[i];
        if (b === 0) {
            if (rx.inFrame && rx.frameLen > 0) {
                /* Copy frame bytes into WASM memory via live M.HEAPU8 view so
                   we are never operating on a detached ArrayBuffer. */
                M.HEAPU8.set(rx.buf.subarray(0, rx.frameLen), frameBufPtr);
                const decLen = M._sensor_cobs_dec(
                    frameBufPtr, rx.frameLen,
                    decodeBufPtr, FRAME_CAP);
                processDecodedFrame(decLen);
            }
            rx.inFrame  = true;
            rx.frameLen = 0;
        } else if (rx.inFrame) {
            if (rx.frameLen < rx.buf.length) {
                rx.buf[rx.frameLen++] = b;
            }
        }
    }
}

/* Protocol state machine (async — runs for the lifetime of the connection) */
async function runProtocol() {
    const delay = ms => new Promise(r => setTimeout(r, ms));

    state.statusStr = 'Waiting for boot loader…';
    await delay(1000);

    /* --- Info loop -------------------------------------------------------- */
    state.proto = 'info';
    const infoCmd = encodeCmd((p, c) => M._sensor_encode_info(p, c));
    console.log('[proto] entering info loop, infoCmd=', infoCmd ? 'ok' : 'null');

    for (let attempt = 0; attempt < 30; attempt++) {
        if (!state.port) return;
        if (infoCmd) state.writer.write(infoCmd).catch(e => console.warn('[proto] write error:', e));
        await delay(200);
        if (state.proto === 'bl_done') break;
    }

    if (state.proto !== 'bl_done') {
        state.statusStr = 'Boot loader not responding.';
        console.warn('[proto] info loop timed out');
        return;
    }

    /* --- bl-done ---------------------------------------------------------- */
    state.statusStr = 'Starting sensor application…';
    const blDoneCmd = encodeCmd((p, c) => M._sensor_encode_bl_done(p, c));
    console.log('[proto] sending bl-done');
    if (blDoneCmd) state.writer.write(blDoneCmd).catch(e => console.warn('[proto] write error:', e));

    await delay(2000);
    if (!state.port) return;

    /* --- Poll loop (request-reply) ----------------------------------------
       Instead of a fixed timer (which suffers from Chrome scheduler jitter),
       processDecodedFrame sends the next poll the instant a response arrives.
       This loop just kicks off the first poll then acts as a watchdog,
       resending if the device goes quiet for more than 1.5 s. */
    state.proto      = 'polling';
    state.statusStr  = 'Connected';
    state.lastRateTs = performance.now();
    console.log('[proto] entering poll loop (request-reply)');

    state.pollCmd    = encodeCmd((p, c) => M._sensor_encode_poll(p, c));
    state.lastPollAt = performance.now();
    if (state.pollCmd) {
        state.writer.write(state.pollCmd)
            .catch(e => console.warn('[proto] poll write error:', e));
    }

    while (state.port) {
        await delay(1500);
        if (!state.port) break;
        if (performance.now() - state.lastPollAt > 1400) {
            console.warn('[proto] watchdog: resending poll');
            state.lastPollAt = performance.now();
            if (state.pollCmd && state.writer) {
                state.writer.write(state.pollCmd)
                    .catch(e => console.warn('[proto] poll write error:', e));
            }
        }
    }
    state.pollCmd = null;
}

async function connect() {
    if (!('serial' in navigator)) {
        state.statusStr =
            'Web Serial API not available — use Chrome 89+ or Edge 89+.  Safari is not supported.';
        updateHeader();
        return;
    }
    if (!M) {
        state.statusStr = 'WASM module not ready — please wait a moment and try again.';
        updateHeader();
        return;
    }

    try {
        state.port = await navigator.serial.requestPort();
        await state.port.open({
            baudRate:    1000000,
            dataBits:    8,
            stopBits:    1,
            parity:      'none',
            flowControl: 'none',
        });
    } catch (e) {
        state.statusStr = `Connect failed: ${e.message}`;
        state.port = null;
        updateHeader();
        return;
    }

    /* Reset state */
    rx.inFrame = false; rx.frameLen = 0;
    state.fftData.fill(0); state.presData.fill(0);
    state.tempStr  = ''; state.versionStr = '';
    state.parseErrors = 0; state.pollCount = 0;
    state.rateFilt = 0;

    document.getElementById('connect-btn').textContent = 'Disconnect';

    state.writer = state.port.writable.getWriter();

    /* Read loop (runs in background) */
    (async () => {
        const reader = state.port.readable.getReader();
        try {
            while (true) {
                const { value, done } = await reader.read();
                if (done) break;
                try {
                    feedBytes(value);
                } catch (e) {
                    /* Parse errors must not kill the connection. */
                    console.error('[rx] feedBytes error:', e);
                    state.parseErrors++;
                }
            }
        } catch (e) {
            /* Serial port error — device disconnected or OS-level error. */
            console.warn('[rx] port error:', e?.message ?? e);
        } finally {
            reader.releaseLock();
        }
        await disconnect();
    })();

    runProtocol();
}

async function disconnect() {
    state.proto = 'idle';
    if (state.writer) {
        try { state.writer.releaseLock(); } catch { /* ignore */ }
        state.writer = null;
    }
    if (state.port) {
        try { await state.port.close(); } catch { /* ignore */ }
        state.port = null;
    }
    state.statusStr = 'Disconnected';
    document.getElementById('connect-btn').textContent = 'Connect';
    updateHeader();
}

/* =========================================================================
   Render loop
   ========================================================================= */

const fftCanvas  = document.getElementById('fft-canvas');
const presCanvas = document.getElementById('pres-canvas');
const fftCtx     = fftCanvas.getContext('2d');
const presCtx    = presCanvas.getContext('2d');

function resizeCanvases() {
    const container = document.getElementById('plots');
    const totalH    = container.clientHeight;
    const w         = container.clientWidth;
    const half      = Math.floor((totalH - 6) / 2);

    fftCanvas.width   = w;    fftCanvas.height   = half;
    presCanvas.width  = w;    presCanvas.height  = half;
}

function updateHeader() {
    document.getElementById('status-text').textContent = state.statusStr;
    document.getElementById('temp-text').textContent   = state.tempStr;
    document.getElementById('rate-text').textContent   =
        state.rateFilt > 0
            ? `${state.rateFilt.toFixed(1)}/s | errors: ${state.parseErrors}`
            : '';
    document.getElementById('version-text').textContent = state.versionStr;
}

/* Cap canvas redraws to ~30 fps — the serial loop runs independently and
   doesn't need to block on vsync.  This halves main-thread canvas work and
   reduces the amount of GC-triggering string allocation per second. */
let _lastDraw = 0;
function renderFrame(ts) {
    if (ts - _lastDraw >= 33) {
        const sr = M ? M._sensor_sample_rate() : 3989;

        drawPlot(fftCtx,
            state.fftData, 0, sr / 2, -100, 50,
            '#00d4ff', 'Frequency [Hz]', 'dB Pa',
            state.fftLabel);

        drawPlot(presCtx,
            state.presData, 0, 1000, -200, 2600,
            '#00b894', 'Time [ms]', 'kPa',
            state.presLabel);

        updateHeader();
        _lastDraw = ts;
    }
    requestAnimationFrame(renderFrame);
}

/* =========================================================================
   Init
   ========================================================================= */

window.addEventListener('resize', () => { resizeCanvases(); });

document.getElementById('connect-btn').addEventListener('click', () => {
    if (state.port) disconnect(); else connect();
});

resizeCanvases();

loadWasm().then(() => {
    allocateWasmBuffers();
    renderFrame();
}).catch(e => {
    document.getElementById('status-text').textContent =
        `Failed to load WASM module: ${e.message}`;
});
