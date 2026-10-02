/* capture_session.c - records the whole boot sequence to an AVI (feature: recboot)
 *
 * Armed by dropping \EFI\visor\rec.arm on the ESP: Visor deletes the marker on
 * the next boot and records from the first frame of the fade-in through to the
 * hand-off, sound included. One file, one boot, nothing to switch back off.
 *
 * Frames are taken from gui_present(), which every fade, animation and menu
 * redraw already goes through, so the recording is literally what was on
 * screen. Audio comes from the HDA tap - the samples the controller was
 * playing, sliced to the same wall-clock window as the frames - so the boot
 * sound lands where it actually played instead of being replayed after.
 */
#include "capture_rec.h"
#include "capture_internal.h"
#include "arch.h"
#include "hda.h"

#define REC_MARKER      L"\\EFI\\visor\\rec.arm"
#define REC_SHOTS_DIR   L"\\EFI\\visor\\shots"

#define REC_FPS_DEF      20u
#define REC_FPS_MIN       5u
#define REC_FPS_MAX      30u
#define REC_WIDTH_DEF   960u
#define REC_WIDTH_MIN   160u
#define REC_WIDTH_MAX  1920u
#define REC_QUALITY_DEF  80u

/* Stop cleanly before the muxer's own ceilings: 32k index slots at two chunks
 * a frame, and AVI's 32-bit sizes. A run that long is a stuck machine, not a
 * recording anyone wants. */
#define REC_MAX_FRAMES  24000u
#define REC_MAX_BYTES   (1536ull * 1024ull * 1024ull)

static int      g_armed;
static int      g_running;
static int      g_done;
static cap_avi *g_avi;
static cap_jpeg *g_jpeg;
static UINT32  *g_scaled;
static INT16   *g_pcm;
static UINTN    g_out_w, g_out_h;
static UINTN    g_fps    = REC_FPS_DEF;
static UINTN    g_width  = REC_WIDTH_DEF;
static UINTN    g_quality = REC_QUALITY_DEF;
static UINT64   g_start_us;
static UINT64   g_interval_us;
static UINTN    g_frames;
static UINTN    g_real_frames;
static UINT64   g_audio_done;      /* audio frames already written */

static UINTN rec_clamp(UINTN v, UINTN lo, UINTN hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- arming ---------------------------------------------------------------
 * The marker may carry overrides - "fps=15 width=1280 quality=90" - so a
 * capture can be retuned without rebuilding or touching boot.conf.
 */

static UINTN rec_read_num(const UINT8 *d, UINTN size, const char *key) {
    UINTN klen = 0;
    while (key[klen]) klen++;
    for (UINTN i = 0; i + klen + 1 < size; i++) {
        UINTN k = 0;
        while (k < klen && (CHAR8)d[i + k] == (CHAR8)key[k]) k++;
        if (k != klen) continue;
        UINTN p = i + klen;
        while (p < size && (d[p] == ' ' || d[p] == '\t')) p++;
        if (p >= size || d[p] != '=') continue;
        p++;
        while (p < size && (d[p] == ' ' || d[p] == '\t')) p++;
        UINTN v = 0;
        int got = 0;
        while (p < size && d[p] >= '0' && d[p] <= '9') {
            v = v * 10 + (UINTN)(d[p] - '0');
            got = 1;
            p++;
            if (v > 100000) break;
        }
        if (got) return v;
    }
    return 0;
}

static void rec_read_options(void) {
    efi_file_buffer_t *fb = efi_load_file(REC_MARKER);
    if (!fb) return;
    if (fb->data && fb->size) {
        const UINT8 *d = (const UINT8*)fb->data;
        UINTN v;
        if ((v = rec_read_num(d, fb->size, "fps")))
            g_fps = rec_clamp(v, REC_FPS_MIN, REC_FPS_MAX);
        if ((v = rec_read_num(d, fb->size, "width")))
            g_width = rec_clamp(v, REC_WIDTH_MIN, REC_WIDTH_MAX);
        if ((v = rec_read_num(d, fb->size, "quality")))
            g_quality = rec_clamp(v, 20, 100);
        efi_free_pool(fb->data);
    }
    efi_free_pool(fb);
}

static void rec_marker_clear(void) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return;
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, REC_MARKER,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(st) && f) f->Delete(f);
    root->Close(root);
}

void cap_session_arm(void) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return;
    int found = efi_file_exists_root(root, REC_MARKER);
    root->Close(root);
    if (!found) return;

    rec_read_options();
    /* Clear the marker before recording, not after: a boot that hangs or
     * loses power halfway must not arm itself again forever. */
    rec_marker_clear();

    g_armed = 1;
    g_interval_us = 1000000ull / g_fps;

    CHAR16 msg[160];
    SPrint(msg, sizeof(msg),
           L"rec: armed - %d fps, up to %d px wide, quality %d",
           (int)g_fps, (int)g_width, (int)g_quality);
    efi_log(msg);
}

int cap_session_active(void) { return g_running; }

/* --- frame plumbing ------------------------------------------------------- */

static void rec_downscale(const UINT32 *src, UINTN sw, UINTN sh,
                          UINT32 *dst, UINTN dw, UINTN dh) {
    for (UINTN j = 0; j < dh; j++) {
        UINTN sy0 = j * sh / dh;
        UINTN sy1 = (j + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sh) sy1 = sh;
        for (UINTN i = 0; i < dw; i++) {
            UINTN sx0 = i * sw / dw;
            UINTN sx1 = (i + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > sw) sx1 = sw;

            UINT32 ar = 0, ag = 0, ab = 0, n = 0;
            for (UINTN sy = sy0; sy < sy1; sy++) {
                const UINT32 *row = src + sy * sw;
                for (UINTN sx = sx0; sx < sx1; sx++) {
                    UINT32 p = row[sx];
                    ar += (p >> 16) & 0xFF;
                    ag += (p >> 8) & 0xFF;
                    ab += p & 0xFF;
                    n++;
                }
            }
            if (!n) n = 1;
            dst[j * dw + i] = ((ar / n) << 16) | ((ag / n) << 8) | (ab / n);
        }
    }
}

static void rec_stop(CHAR16 *why) {
    if (g_avi) {
        cap_avi_close(g_avi);
        g_avi = NULL;
    }
    if (g_jpeg)   { cap_jpeg_free(g_jpeg); g_jpeg = NULL; }
    if (g_scaled) { efi_free_pool(g_scaled); g_scaled = NULL; }
    if (g_pcm)    { efi_free_pool(g_pcm); g_pcm = NULL; }
    g_running = 0;
    g_armed = 0;
    g_done = 1;
    if (why) efi_log(why);
}

static int rec_start(UINTN w, UINTN h) {
    UINTN ow = w, oh = h;
    if (ow > g_width) {
        oh = (h * g_width + w / 2) / w;
        ow = g_width;
    }
    /* 4:2:0 chroma wants even dimensions */
    ow &= ~(UINTN)1;
    oh &= ~(UINTN)1;
    if (!ow || !oh) return 0;

    if (!EFI_ERROR(cap_ensure_dir(L"\\EFI\\visor")))
        cap_ensure_dir(REC_SHOTS_DIR);

    CHAR16 name[192], path[256];
    cap_timestamp_name(name, 192, L"boot", L".avi");
    SPrint(path, sizeof(path), REC_SHOTS_DIR L"\\%s", name);

    g_jpeg = cap_jpeg_new(ow, oh, g_quality);
    if (!g_jpeg) return 0;

    if (ow != w || oh != h) {
        g_scaled = efi_allocate_pool(ow * oh * sizeof(UINT32));
        if (!g_scaled) { cap_jpeg_free(g_jpeg); g_jpeg = NULL; return 0; }
    }

    /* One frame's worth of stereo samples, plus slack for the rounding when
     * fps does not divide the sample rate evenly. */
    UINTN pcm_max = (UINTN)HDA_SAMPLE_RATE / g_fps + 2;
    g_pcm = efi_allocate_pool(pcm_max * HDA_CHANNELS * sizeof(INT16));
    if (!g_pcm) {
        cap_jpeg_free(g_jpeg); g_jpeg = NULL;
        if (g_scaled) { efi_free_pool(g_scaled); g_scaled = NULL; }
        return 0;
    }

    g_avi = cap_avi_new(path, ow, oh, g_fps, HDA_SAMPLE_RATE, HDA_CHANNELS);
    if (!g_avi) {
        cap_jpeg_free(g_jpeg); g_jpeg = NULL;
        if (g_scaled) { efi_free_pool(g_scaled); g_scaled = NULL; }
        efi_free_pool(g_pcm); g_pcm = NULL;
        return 0;
    }

    g_out_w = ow;
    g_out_h = oh;
    g_start_us = arch_now_us();
    g_frames = 0;
    g_real_frames = 0;
    g_audio_done = 0;
    g_running = 1;

    CHAR16 msg[192];
    SPrint(msg, sizeof(msg), L"rec: recording %s at %d x %d", name,
           (int)ow, (int)oh);
    efi_log(msg);
    return 1;
}

/* Audio for slot k: the samples that were playing during that frame's window.
 * Slot edges are computed from the frame index so rounding never accumulates
 * into drift, and anything outside a playing stream is silence. */
static int rec_audio_slot(UINTN slot) {
    UINT64 s0 = (UINT64)slot * HDA_SAMPLE_RATE / g_fps;
    UINT64 s1 = (UINT64)(slot + 1) * HDA_SAMPLE_RATE / g_fps;
    UINTN n = (UINTN)(s1 - s0);
    if (!n) return 1;

    const INT16 *pcm = NULL;
    UINTN frames = 0;
    UINT64 start_us = 0;
    int playing = hda_tap(&pcm, &frames, &start_us);

    if (!playing) {
        for (UINTN i = 0; i < n * HDA_CHANNELS; i++) g_pcm[i] = 0;
    } else {
        UINT64 t0 = g_start_us + (UINT64)slot * g_interval_us;
        INT64 rel = (INT64)t0 - (INT64)start_us;
        INT64 base = rel * (INT64)HDA_SAMPLE_RATE / 1000000;
        for (UINTN i = 0; i < n; i++) {
            INT64 s = base + (INT64)i;
            if (s >= 0 && s < (INT64)frames) {
                g_pcm[i * HDA_CHANNELS]     = pcm[s * HDA_CHANNELS];
                g_pcm[i * HDA_CHANNELS + 1] = pcm[s * HDA_CHANNELS + 1];
            } else {
                g_pcm[i * HDA_CHANNELS]     = 0;
                g_pcm[i * HDA_CHANNELS + 1] = 0;
            }
        }
    }

    if (!cap_avi_audio(g_avi, g_pcm, n)) return 0;
    g_audio_done += n;
    return 1;
}

void cap_session_frame(const UINT32 *pixels, UINTN w, UINTN h) {
    if (!g_armed || g_done || !pixels || !w || !h) return;

    if (!g_running && !rec_start(w, h)) {
        rec_stop(L"WARN: rec: could not start the recording - carrying on silently");
        return;
    }

    /* The screen mode changed under us; the container cannot follow. */
    if (g_scaled == NULL && (w != g_out_w || h != g_out_h)) {
        rec_stop(L"rec: screen geometry changed - closing the recording here");
        return;
    }

    UINT64 now = arch_now_us();
    UINTN slot = (UINTN)((now - g_start_us) / g_interval_us);
    if (slot < g_frames) return;              /* this frame slot is already in */

    if (g_frames >= REC_MAX_FRAMES || cap_avi_bytes(g_avi) >= REC_MAX_BYTES) {
        rec_stop(L"rec: hit the recording size limit - saving what we have");
        return;
    }

    /* Idle slots since the last frame: the picture did not change, so store
     * them as duplicates rather than re-encoding the same image. */
    for (UINTN k = g_frames; k < slot; k++) {
        if (!cap_avi_video_dup(g_avi, 1) || !rec_audio_slot(k)) {
            rec_stop(L"WARN: rec: write failed - saving what we have");
            return;
        }
    }
    g_frames = slot;

    const UINT32 *src = pixels;
    if (g_scaled) {
        rec_downscale(pixels, w, h, g_scaled, g_out_w, g_out_h);
        src = g_scaled;
    }

    UINTN len = 0;
    const UINT8 *jpeg = cap_jpeg_frame(g_jpeg, src, &len);
    if (!jpeg || !cap_avi_video(g_avi, jpeg, len) || !rec_audio_slot(slot)) {
        rec_stop(L"WARN: rec: write failed - saving what we have");
        return;
    }
    g_frames = slot + 1;
    g_real_frames++;
}

void cap_session_finish(void) {
    if (!g_running || g_done) {
        g_armed = 0;
        g_done = 1;
        return;
    }

    UINTN frames = g_frames;
    UINTN real = g_real_frames;
    UINT64 bytes = cap_avi_bytes(g_avi);
    UINTN secs = g_fps ? frames / g_fps : 0;

    if (g_avi) {
        EFI_STATUS st = cap_avi_close(g_avi);
        g_avi = NULL;
        if (EFI_ERROR(st)) efi_log(L"WARN: rec: the recording did not close cleanly");
    }
    if (g_jpeg)   { cap_jpeg_free(g_jpeg); g_jpeg = NULL; }
    if (g_scaled) { efi_free_pool(g_scaled); g_scaled = NULL; }
    if (g_pcm)    { efi_free_pool(g_pcm); g_pcm = NULL; }

    g_running = 0;
    g_armed = 0;
    g_done = 1;

    CHAR16 msg[192];
    SPrint(msg, sizeof(msg),
           L"rec: saved %d frames (%d encoded) - %ds, %d KiB",
           (int)frames, (int)real, (int)secs, (int)(bytes / 1024));
    efi_log(msg);
}
