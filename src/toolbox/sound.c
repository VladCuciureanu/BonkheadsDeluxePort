/* Sound Manager: sampled-sound channels mixed on the main thread into an
 * SDL3 audio stream. Mixing on the main thread lets channel callbacks
 * (callBackCmd) call straight into the recompiled game code. */
#include "toolbox.h"
#include "sound.h"
#include "event.h"
#include "platform/platform.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#define OUT_RATE 44100
#define QLEN 128
#define MAXCH 16
#define TARGET_FRAMES 2048    /* ~46 ms of buffered audio */

enum { nullCmd = 0, quietCmd = 3, flushCmd = 4, reInitCmd = 5, waitCmd = 10, callBackCmd = 13, syncCmd = 14,
       ampCmd = 43, volumeCmd = 46, soundCmd = 80, bufferCmd = 81 };

typedef struct { uint16_t cmd; int16_t p1; uint32_t p2; } Cmd;

typedef struct {
    bool used;
    uint32_t chan;          /* emulated SndChannel record */
    uint32_t callback;      /* userRoutine */
    Cmd q[QLEN];
    int qh, qn;
    /* current playback */
    bool playing;
    const uint8_t *src;     /* host pointer into emulated memory snapshot */
    uint8_t *own;           /* copy of sample data */
    uint32_t frames;
    int bits;               /* 8 or 16 */
    int nch;
    uint64_t pos;           /* 32.32 fixed point frame position */
    uint64_t step;
    int wait_frames;
    int amp;                /* 0..255 */
    int vol_l, vol_r;       /* 0..256 */
} Chan;

static Chan chans[MAXCH];
static SDL_AudioStream *stream;
static bool enabled;
static int master_l = 256, master_r = 256;
static bool in_update;

static Chan *find_chan(uint32_t c)
{
    for (int i = 0; i < MAXCH; i++) if (chans[i].used && chans[i].chan == c) return &chans[i];
    return NULL;
}

void sound_init(bool en)
{
    enabled = en;
    if (!enabled) return;
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, OUT_RATE };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) { log_warn("audio disabled: %s", SDL_GetError()); enabled = false; return; }
    SDL_ResumeAudioStreamDevice(stream);
}

/* parse a sound header at emulated address hdr; sets up channel playback */
static bool start_buffer(Chan *c, uint32_t hdr)
{
    uint32_t sampleptr = rdl(hdr);
    uint32_t rate = rdl(hdr + 8);
    uint8_t encode = (uint8_t)rdb(hdr + 20);
    uint32_t data, frames; int bits = 8, nch = 1;
    if (encode == 0) {
        frames = rdl(hdr + 4);
        data = sampleptr ? sampleptr : hdr + 22;
    } else if (encode == 0xFF) {
        nch = (int)rdl(hdr + 4);
        frames = rdl(hdr + 22);
        bits = (int16_t)rdw(hdr + 48);
        data = sampleptr ? sampleptr : hdr + 64;
    } else {
        log_warn("compressed sound (encode %d) not supported", encode);
        return false;
    }
    if (nch < 1 || nch > 2 || (bits != 8 && bits != 16) || frames > 50000000) return false;
    size_t bytes = (size_t)frames * nch * (bits / 8);
    free(c->own);
    c->own = (uint8_t *)malloc(bytes ? bytes : 1);
    memcpy(c->own, HP(data), bytes);
    c->src = c->own;
    c->frames = frames; c->bits = bits; c->nch = nch;
    c->pos = 0;
    double r = rate / 65536.0;
    if (r < 1000) r = 22254.5;
    c->step = (uint64_t)(r / OUT_RATE * 4294967296.0);
    c->playing = true;
    return true;
}

static void call_user_routine(Chan *c, Cmd cmd)
{
    if (!c->callback) return;
    uint32_t cmdp = mm_new_ptr(8, false);
    wrw(cmdp, cmd.cmd); wrw(cmdp + 2, (uint16_t)cmd.p1); wrl(cmdp + 4, cmd.p2);
    uint32_t sd[8], sa[8]; memcpy(sd, D, sizeof sd); memcpy(sa, A, sizeof sa);
    uint8_t f[5] = { fN, fZ, fV, fC, fX };
    PUSH32(c->chan); PUSH32(cmdp);
    call_emulated(c->callback);
    memcpy(D, sd, sizeof sd); memcpy(A, sa, sizeof sa);
    fN = f[0]; fZ = f[1]; fV = f[2]; fC = f[3]; fX = f[4];
    mm_dispose_ptr(cmdp);
}

static bool exec_cmd(Chan *c, Cmd cmd, bool immediate)
{
    switch (cmd.cmd & 0x7FFF) {
    case nullCmd: return true;
    case quietCmd: c->playing = false; c->wait_frames = 0; return true;
    case flushCmd: c->qn = 0; c->qh = 0; return true;
    case reInitCmd: return true;
    case waitCmd: c->wait_frames = (int)((long)cmd.p1 * OUT_RATE / 2000); return true;
    case callBackCmd: call_user_routine(c, cmd); return true;
    case ampCmd: c->amp = cmd.p1 > 255 ? 255 : cmd.p1 < 0 ? 0 : cmd.p1; return true;
    case volumeCmd: c->vol_l = (int)(cmd.p2 & 0xFFFF); c->vol_r = (int)(cmd.p2 >> 16); return true;
    case soundCmd: case bufferCmd:
        (void)immediate;
        return start_buffer(c, cmd.p2);
    default:
        return true;
    }
}

/* advance a channel's queue until it is playing/waiting or empty */
static void pump_queue(Chan *c)
{
    while (!c->playing && c->wait_frames <= 0 && c->qn > 0) {
        Cmd cmd = c->q[c->qh];
        c->qh = (c->qh + 1) % QLEN; c->qn--;
        exec_cmd(c, cmd, false);
    }
}

static bool enqueue(Chan *c, Cmd cmd)
{
    if (c->qn >= QLEN) return false;
    c->q[(c->qh + c->qn) % QLEN] = cmd;
    c->qn++;
    return true;
}

static void mix(int16_t *out, int frames)
{
    int32_t *acc = (int32_t *)calloc((size_t)frames * 2, sizeof(int32_t));
    for (int ci = 0; ci < MAXCH; ci++) {
        Chan *c = &chans[ci];
        if (!c->used) continue;
        int f = 0;
        while (f < frames) {
            pump_queue(c);
            if (c->wait_frames > 0) { int n = c->wait_frames < frames - f ? c->wait_frames : frames - f; c->wait_frames -= n; f += n; continue; }
            if (!c->playing) break;
            int gain_l = c->amp * c->vol_l / 256, gain_r = c->amp * c->vol_r / 256;
            for (; f < frames; f++) {
                uint32_t idx = (uint32_t)(c->pos >> 32);
                if (idx >= c->frames) { c->playing = false; break; }
                int sl, sr;
                if (c->bits == 8) {
                    sl = ((int)c->src[idx * c->nch] - 128) << 8;
                    sr = c->nch == 2 ? ((int)c->src[idx * 2 + 1] - 128) << 8 : sl;
                } else {
                    const uint8_t *p = c->src + (size_t)idx * c->nch * 2;
                    sl = (int16_t)((p[0] << 8) | p[1]);
                    sr = c->nch == 2 ? (int16_t)((p[2] << 8) | p[3]) : sl;
                }
                acc[f * 2] += sl * gain_l / 255;
                acc[f * 2 + 1] += sr * gain_r / 255;
                c->pos += c->step;
            }
        }
    }
    for (int i = 0; i < frames; i++) {
        int32_t l = acc[i * 2] * master_l / 256, r = acc[i * 2 + 1] * master_r / 256;
        out[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    free(acc);
}

void sound_update(void)
{
    if (in_update) return;
    in_update = true;
    if (!enabled) {
        /* still run queues so callbacks/sync plays complete */
        for (int i = 0; i < MAXCH; i++) if (chans[i].used) { chans[i].playing = false; chans[i].wait_frames = 0; pump_queue(&chans[i]); chans[i].playing = false; }
        in_update = false;
        return;
    }
    int queued = SDL_GetAudioStreamQueued(stream) / 4;
    if (queued < TARGET_FRAMES) {
        int n = TARGET_FRAMES - queued;
        if (n < 256) n = 256;
        int16_t *buf = (int16_t *)malloc((size_t)n * 4);
        mix(buf, n);
        SDL_PutAudioStreamData(stream, buf, n * 4);
        free(buf);
    }
    in_update = false;
}

static bool chan_busy(Chan *c) { return c->playing || c->qn > 0 || c->wait_frames > 0; }

static Chan *alloc_chan(uint32_t rec)
{
    for (int i = 0; i < MAXCH; i++) if (!chans[i].used) {
        Chan *c = &chans[i];
        memset(c, 0, sizeof *c);
        c->used = true; c->chan = rec; c->amp = 255; c->vol_l = c->vol_r = 256;
        return c;
    }
    return NULL;
}

/* ------------------------------------------------------------ traps */
static void T_SndNewChannel(uint16_t op)
{
    (void)op;
    uint32_t proc = ARG32(); (void)ARG32(); (void)ARG16(); uint32_t pchan = ARG32();
    uint32_t rec = rdl(pchan);
    if (!rec) { rec = mm_new_ptr(36 + QLEN * 8, true); wrl(pchan, rec); }
    wrw(rec + 30, QLEN);
    wrl(rec + 8, proc);
    Chan *c = alloc_chan(rec);
    if (!c) { RET16(-205); return; }
    c->callback = proc;
    RET16(0);
}

static void T_SndDisposeChannel(uint16_t op)
{
    (void)op;
    bool quiet = ARG8() != 0; uint32_t rec = ARG32();
    Chan *c = find_chan(rec);
    if (c) {
        if (!quiet) while (chan_busy(c) && !evt_quit_requested()) { evt_idle(); plat_sleep_ms(2); }
        free(c->own);
        memset(c, 0, sizeof *c);
    }
    RET16(0);
}

static Cmd read_cmd(uint32_t p) { Cmd c = { (uint16_t)rdw(p), (int16_t)rdw(p + 2), rdl(p + 4) }; return c; }

static void T_SndDoCommand(uint16_t op)
{
    (void)op;
    (void)ARG8(); uint32_t cmdp = ARG32(); uint32_t rec = ARG32();
    Chan *c = find_chan(rec);
    if (!c) { RET16(-205); return; }
    Cmd cmd = read_cmd(cmdp);
    if ((cmd.cmd & 0x7FFF) == flushCmd || (cmd.cmd & 0x7FFF) == quietCmd) { exec_cmd(c, cmd, true); RET16(0); return; }
    RET16(enqueue(c, cmd) ? 0 : -203 /* queueFull */);
}

static void T_SndDoImmediate(uint16_t op)
{
    (void)op;
    uint32_t cmdp = ARG32(); uint32_t rec = ARG32();
    Chan *c = find_chan(rec);
    if (!c) { RET16(-205); return; }
    exec_cmd(c, read_cmd(cmdp), true);
    RET16(0);
}

/* parse a 'snd ' resource and queue its commands; returns false on error */
static bool queue_snd(Chan *c, uint32_t h)
{
    uint32_t base = rdl(h);
    if (!base) return false;
    uint16_t fmt = (uint16_t)rdw(base);
    uint32_t p;
    if (fmt == 1) {
        int nmods = (int16_t)rdw(base + 2);
        p = base + 4 + (uint32_t)nmods * 6;
    } else if (fmt == 2) {
        p = base + 4;
    } else return false;
    int ncmds = (int16_t)rdw(p); p += 2;
    for (int i = 0; i < ncmds; i++, p += 8) {
        Cmd cmd = read_cmd(p);
        if (cmd.cmd & 0x8000) { cmd.p2 += base; cmd.cmd &= 0x7FFF; }
        if (!enqueue(c, cmd)) return false;
    }
    return true;
}

static void T_SndPlay(uint16_t op)
{
    (void)op;
    bool async = ARG8() != 0; uint32_t h = ARG32(); uint32_t rec = ARG32();
    Chan *c = rec ? find_chan(rec) : NULL;
    bool temp = false;
    if (!c) {
        c = alloc_chan(0);
        if (!c) { RET16(-205); return; }
        temp = true; async = false;
    }
    if (!queue_snd(c, h)) { if (temp) memset(c, 0, sizeof *c); RET16(-206 /* badFormat */); return; }
    pump_queue(c);
    if (!async) {
        while (chan_busy(c) && !evt_quit_requested()) { evt_idle(); plat_sleep_ms(2); }
    }
    if (temp) { free(c->own); memset(c, 0, sizeof *c); }
    RET16(0);
}

static void T_SndControl(uint16_t op) { (void)op; (void)ARG32(); (void)ARG16(); RET16(0); }
static void T_SndAddModifier(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); (void)ARG16(); (void)ARG32(); RET16(0); }

static void T_SoundDispatch(uint16_t op)
{
    (void)op;
    uint32_t sel = D[0];
    switch (sel) {
    case 0x022C0018: { uint32_t p = ARG32(); wrl(p, ((uint32_t)master_r << 16) | (uint32_t)master_l); RET16(0); return; }
    case 0x02300018: { uint32_t v = ARG32(); master_l = (int)(v & 0xFFFF); master_r = (int)(v >> 16); if (master_l > 256) master_l = 256; if (master_r > 256) master_r = 256; RET16(0); return; }
    case 0x000C0008: RET32(0x03208000); return;  /* SndSoundManagerVersion 3.2 */
    case 0x00100008: RET32(0x03208000); return;  /* MACEVersion */
    default:
        log_warn("SoundDispatch %08X ignored", sel);
        A[7] += (sel >> 16) & 0xFFFF;  /* param bytes in high word for most selectors */
        RET16(0);
    }
}

void sound_beep(void)
{
    /* short square-wave beep through a temporary buffer */
    if (!enabled) return;
    int n = OUT_RATE / 8;
    int16_t *buf = (int16_t *)malloc((size_t)n * 4);
    for (int i = 0; i < n; i++) { int16_t v = ((i / 50) & 1) ? 6000 : -6000; buf[2 * i] = buf[2 * i + 1] = v; }
    SDL_PutAudioStreamData(stream, buf, n * 4);
    free(buf);
}

static void T_SysBeep(uint16_t op) { (void)op; (void)ARG16(); sound_beep(); }

void sound_register_traps(void)
{
    trap_register(0xA807, T_SndNewChannel, "SndNewChannel");
    trap_register(0xA801, T_SndDisposeChannel, "SndDisposeChannel");
    trap_register(0xA803, T_SndDoCommand, "SndDoCommand");
    trap_register(0xA804, T_SndDoImmediate, "SndDoImmediate");
    trap_register(0xA805, T_SndPlay, "SndPlay");
    trap_register(0xA806, T_SndControl, "SndControl");
    trap_register(0xA802, T_SndAddModifier, "SndAddModifier");
    trap_register(0xA800, T_SoundDispatch, "SoundDispatch");
    trap_register(0xA9C8, T_SysBeep, "SysBeep");
}
