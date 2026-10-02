/* capture_avi.c - RIFF/AVI muxer for boot recordings (feature: recboot)
 *
 * MJPG video plus optional 16-bit PCM audio, written as the recording happens:
 * the header goes out with placeholder sizes, chunks are appended, and the
 * sizes, frame counts and the idx1 index are patched in at close.
 *
 * Idle stretches of the menu are stored as zero-length video chunks - the AVI
 * way of saying "same picture as before" - so a menu sitting still costs 8
 * bytes a frame instead of a re-encode, and wall-clock timing still lines up
 * with the audio.
 */
#include "capture_internal.h"

#define AVI_MAX_INDEX   (64u * 1024u)   /* 16 bytes each: 1 MiB ceiling */

#define AVIF_HASINDEX       0x00000010u
#define AVIF_ISINTERLEAVED  0x00000100u
#define AVIIF_KEYFRAME      0x00000010u

typedef struct {
    UINT32 id;
    UINT32 flags;
    UINT32 offset;
    UINT32 size;
} avi_idx;

struct cap_avi {
    cap_file *f;
    UINTN   w, h, fps;
    UINTN   arate, achan;
    int     have_audio;

    avi_idx *idx;
    UINTN    nidx;

    UINTN   frames;          /* video frames, duplicates included */
    UINT64  asamples;        /* audio frames written */
    UINTN   max_chunk;
    UINT64  movi_start;      /* file offset of the 'movi' FOURCC */
    int     err;

    /* header fields patched at close */
    UINT64  pos_riff_size;
    UINT64  pos_movi_size;
    UINT64  pos_total_frames;
    UINT64  pos_max_rate;
    UINT64  pos_vid_buf;
    UINT64  pos_vid_len;
    UINT64  pos_aud_len;
};

static UINT32 avi_fourcc(const char *s) {
    return (UINT32)(UINT8)s[0] | ((UINT32)(UINT8)s[1] << 8) |
           ((UINT32)(UINT8)s[2] << 16) | ((UINT32)(UINT8)s[3] << 24);
}

static int avi_put(cap_avi *a, const void *p, UINTN n) {
    if (a->err) return 0;
    if (!cap_file_write(a->f, p, n)) { a->err = 1; return 0; }
    return 1;
}

static int avi_u32(cap_avi *a, UINT32 v) {
    UINT8 t[4] = { (UINT8)v, (UINT8)(v >> 8), (UINT8)(v >> 16), (UINT8)(v >> 24) };
    return avi_put(a, t, 4);
}

static int avi_u16(cap_avi *a, UINT16 v) {
    UINT8 t[2] = { (UINT8)v, (UINT8)(v >> 8) };
    return avi_put(a, t, 2);
}

static int avi_tag(cap_avi *a, const char *s) {
    return avi_put(a, s, 4);
}

static int avi_patch_u32(cap_avi *a, UINT64 pos, UINT32 v) {
    UINT8 t[4] = { (UINT8)v, (UINT8)(v >> 8), (UINT8)(v >> 16), (UINT8)(v >> 24) };
    if (a->err) return 0;
    if (!cap_file_write_at(a->f, pos, t, 4)) { a->err = 1; return 0; }
    return 1;
}

static void avi_header(cap_avi *a) {
    UINT32 usec = a->fps ? (UINT32)(1000000u / a->fps) : 50000u;
    UINT32 streams = a->have_audio ? 2u : 1u;
    UINT32 blockalign = (UINT32)(a->achan * 2u);

    /* hdrl content: 'hdrl' + avih + one strl per stream */
    UINT32 vid_strl = 12 + (8 + 56) + (8 + 40);
    UINT32 aud_strl = 12 + (8 + 56) + (8 + 18);
    UINT32 hdrl = 4 + (8 + 56) + vid_strl + (a->have_audio ? aud_strl : 0);

    avi_tag(a, "RIFF");
    a->pos_riff_size = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* patched at close */
    avi_tag(a, "AVI ");

    avi_tag(a, "LIST");
    avi_u32(a, hdrl);
    avi_tag(a, "hdrl");

    avi_tag(a, "avih");
    avi_u32(a, 56);
    avi_u32(a, usec);
    a->pos_max_rate = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* dwMaxBytesPerSec, patched */
    avi_u32(a, 0);                       /* padding granularity */
    avi_u32(a, AVIF_HASINDEX | AVIF_ISINTERLEAVED);
    a->pos_total_frames = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* dwTotalFrames, patched */
    avi_u32(a, 0);                       /* initial frames */
    avi_u32(a, streams);
    a->pos_vid_buf = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* suggested buffer, patched */
    avi_u32(a, (UINT32)a->w);
    avi_u32(a, (UINT32)a->h);
    for (int i = 0; i < 4; i++) avi_u32(a, 0);

    /* video stream */
    avi_tag(a, "LIST");
    avi_u32(a, vid_strl);
    avi_tag(a, "strl");
    avi_tag(a, "strh");
    avi_u32(a, 56);
    avi_tag(a, "vids");
    avi_tag(a, "MJPG");
    avi_u32(a, 0);                       /* flags */
    avi_u16(a, 0);                       /* priority */
    avi_u16(a, 0);                       /* language */
    avi_u32(a, 0);                       /* initial frames */
    avi_u32(a, 1);                       /* scale */
    avi_u32(a, (UINT32)a->fps);          /* rate */
    avi_u32(a, 0);                       /* start */
    a->pos_vid_len = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* dwLength, patched */
    avi_u32(a, 0);                       /* suggested buffer */
    avi_u32(a, 0xFFFFFFFFu);             /* quality: default */
    avi_u32(a, 0);                       /* sample size: variable */
    avi_u16(a, 0); avi_u16(a, 0);
    avi_u16(a, (UINT16)a->w); avi_u16(a, (UINT16)a->h);

    avi_tag(a, "strf");
    avi_u32(a, 40);
    avi_u32(a, 40);                      /* biSize */
    avi_u32(a, (UINT32)a->w);
    avi_u32(a, (UINT32)a->h);
    avi_u16(a, 1);                       /* planes */
    avi_u16(a, 24);                      /* bit count */
    avi_tag(a, "MJPG");                  /* biCompression */
    avi_u32(a, (UINT32)(a->w * a->h * 3));
    avi_u32(a, 0); avi_u32(a, 0);
    avi_u32(a, 0); avi_u32(a, 0);

    if (!a->have_audio) return;

    /* audio stream */
    avi_tag(a, "LIST");
    avi_u32(a, aud_strl);
    avi_tag(a, "strl");
    avi_tag(a, "strh");
    avi_u32(a, 56);
    avi_tag(a, "auds");
    avi_u32(a, 0);                       /* handler */
    avi_u32(a, 0);                       /* flags */
    avi_u16(a, 0); avi_u16(a, 0);
    avi_u32(a, 0);                       /* initial frames */
    avi_u32(a, 1);                       /* scale: one sample */
    avi_u32(a, (UINT32)a->arate);        /* rate: samples per second */
    avi_u32(a, 0);                       /* start */
    a->pos_aud_len = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* dwLength in samples, patched */
    avi_u32(a, (UINT32)(a->arate * blockalign / 2));
    avi_u32(a, 0xFFFFFFFFu);
    avi_u32(a, blockalign);              /* sample size */
    avi_u16(a, 0); avi_u16(a, 0); avi_u16(a, 0); avi_u16(a, 0);

    avi_tag(a, "strf");
    avi_u32(a, 18);
    avi_u16(a, 1);                       /* WAVE_FORMAT_PCM */
    avi_u16(a, (UINT16)a->achan);
    avi_u32(a, (UINT32)a->arate);
    avi_u32(a, (UINT32)(a->arate * blockalign));
    avi_u16(a, (UINT16)blockalign);
    avi_u16(a, 16);                      /* bits per sample */
    avi_u16(a, 0);                       /* cbSize */
}

cap_avi *cap_avi_new(const CHAR16 *path, UINTN w, UINTN h, UINTN fps,
                     UINTN audio_rate, UINTN audio_channels) {
    if (!path || !w || !h || !fps) return NULL;

    cap_avi *a = efi_allocate_pool(sizeof(cap_avi));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));

    a->w = w; a->h = h; a->fps = fps;
    a->arate = audio_rate;
    a->achan = audio_channels;
    a->have_audio = (audio_rate && audio_channels) ? 1 : 0;

    a->idx = efi_allocate_pool(sizeof(avi_idx) * AVI_MAX_INDEX);
    if (!a->idx) { efi_free_pool(a); return NULL; }

    a->f = cap_file_create(path);
    if (!a->f) {
        efi_free_pool(a->idx);
        efi_free_pool(a);
        return NULL;
    }

    avi_header(a);

    avi_tag(a, "LIST");
    a->pos_movi_size = cap_file_pos(a->f);
    avi_u32(a, 0);                       /* patched at close */
    a->movi_start = cap_file_pos(a->f);  /* the 'movi' FOURCC itself */
    avi_tag(a, "movi");

    if (a->err) {
        cap_file_close(a->f);
        efi_free_pool(a->idx);
        efi_free_pool(a);
        return NULL;
    }
    return a;
}

/* idx1 offsets are relative to the 'movi' FOURCC, the convention every player
 * understands. */
static int avi_index(cap_avi *a, const char *id, UINT32 flags, UINT64 at,
                     UINT32 size) {
    if (a->nidx >= AVI_MAX_INDEX) { a->err = 1; return 0; }
    avi_idx *e = &a->idx[a->nidx++];
    e->id     = avi_fourcc(id);
    e->flags  = flags;
    e->offset = (UINT32)(at - a->movi_start);
    e->size   = size;
    return 1;
}

static int avi_chunk(cap_avi *a, const char *id, UINT32 flags,
                     const void *data, UINTN len) {
    if (a->err) return 0;
    UINT64 at = cap_file_pos(a->f);
    if (!avi_tag(a, id) || !avi_u32(a, (UINT32)len)) return 0;
    if (len && !avi_put(a, data, len)) return 0;
    if (len & 1) {                       /* chunks are word-aligned */
        UINT8 pad = 0;
        if (!avi_put(a, &pad, 1)) return 0;
    }
    if (len > a->max_chunk) a->max_chunk = (UINTN)len;
    return avi_index(a, id, flags, at, (UINT32)len);
}

int cap_avi_video(cap_avi *a, const UINT8 *jpeg, UINTN len) {
    if (!a || a->err || !jpeg || !len) return 0;
    if (!avi_chunk(a, "00dc", AVIIF_KEYFRAME, jpeg, len)) return 0;
    a->frames++;
    return 1;
}

int cap_avi_video_dup(cap_avi *a, UINTN count) {
    if (!a || a->err) return 0;
    for (UINTN i = 0; i < count; i++) {
        /* zero-length frame: "no change since the last one" */
        if (!avi_chunk(a, "00dc", 0, NULL, 0)) return 0;
        a->frames++;
    }
    return 1;
}

int cap_avi_audio(cap_avi *a, const INT16 *pcm, UINTN frames) {
    if (!a || a->err || !a->have_audio || !pcm || !frames) return 0;
    UINTN bytes = frames * a->achan * sizeof(INT16);
    if (!avi_chunk(a, "01wb", AVIIF_KEYFRAME, pcm, bytes)) return 0;
    a->asamples += frames;
    return 1;
}

UINTN  cap_avi_frames(const cap_avi *a) { return a ? a->frames : 0; }
UINT64 cap_avi_bytes(const cap_avi *a)  { return a ? cap_file_pos(a->f) : 0; }
int    cap_avi_failed(const cap_avi *a) { return a ? a->err : 1; }

EFI_STATUS cap_avi_close(cap_avi *a) {
    if (!a) return EFI_INVALID_PARAMETER;

    EFI_STATUS st = EFI_SUCCESS;
    UINT64 movi_end = cap_file_pos(a->f);

    if (!a->err && a->frames) {
        /* idx1 */
        avi_tag(a, "idx1");
        avi_u32(a, (UINT32)(a->nidx * 16));
        for (UINTN i = 0; i < a->nidx && !a->err; i++) {
            avi_u32(a, a->idx[i].id);
            avi_u32(a, a->idx[i].flags);
            avi_u32(a, a->idx[i].offset);
            avi_u32(a, a->idx[i].size);
        }

        UINT64 end = cap_file_pos(a->f);
        UINT64 secs = a->fps ? (a->frames + a->fps - 1) / a->fps : 1;
        if (!secs) secs = 1;

        avi_patch_u32(a, a->pos_riff_size, (UINT32)(end - 8));
        avi_patch_u32(a, a->pos_movi_size,
                      (UINT32)(movi_end - a->movi_start));
        avi_patch_u32(a, a->pos_total_frames, (UINT32)a->frames);
        avi_patch_u32(a, a->pos_vid_len, (UINT32)a->frames);
        avi_patch_u32(a, a->pos_vid_buf, (UINT32)a->max_chunk);
        avi_patch_u32(a, a->pos_max_rate, (UINT32)(movi_end / secs));
        if (a->have_audio)
            avi_patch_u32(a, a->pos_aud_len, (UINT32)a->asamples);
    }

    if (a->err || !a->frames) st = EFI_DEVICE_ERROR;

    cap_file_close(a->f);
    efi_free_pool(a->idx);
    efi_free_pool(a);
    return st;
}
