/* capture_internal.h - shared internals of the src/capture/capture.c translation units. */
#ifndef CAPTURE_INTERNAL_H
#define CAPTURE_INTERNAL_H

#include "capture.h"
#include "efi_helpers.h"

#include <efilib.h>
#include <string.h>

UINT32 cap_crc32(UINT32 crc, const UINT8 *data, UINTN len);
UINT32 cap_adler32(const UINT8 *data, UINTN len);
typedef struct {
    UINT8  *buf;
    UINTN   len;
    UINTN   cap;
} cap_buf;
void cap_buf_free(cap_buf *b);
int cap_buf_reserve(cap_buf *b, UINTN need);
int cap_buf_put(cap_buf *b, const UINT8 *p, UINTN n);
int cap_buf_u32be(cap_buf *b, UINT32 v);
int cap_buf_u16le(cap_buf *b, UINT16 v);

/* Streaming file sink. A boot recording is written as it is captured rather
 * than assembled in RAM, so the encoders need seek-and-patch access to an open
 * file. capture_file.c backs this with EFI_FILE_PROTOCOL; the host harnesses
 * back it with stdio. */
typedef struct cap_file cap_file;

cap_file *cap_file_create(const CHAR16 *path);
int       cap_file_write(cap_file *f, const void *data, UINTN len);
int       cap_file_write_at(cap_file *f, UINT64 pos, const void *data, UINTN len);
UINT64    cap_file_pos(const cap_file *f);
void      cap_file_close(cap_file *f);

/* Baseline JPEG encoder (capture_jpeg.c). */
typedef struct cap_jpeg cap_jpeg;

cap_jpeg    *cap_jpeg_new(UINTN w, UINTN h, UINTN quality);
const UINT8 *cap_jpeg_frame(cap_jpeg *j, const UINT32 *pixels, UINTN *out_len);
UINTN        cap_jpeg_width(const cap_jpeg *j);
UINTN        cap_jpeg_height(const cap_jpeg *j);
void         cap_jpeg_free(cap_jpeg *j);

/* AVI muxer (capture_avi.c): MJPG video plus optional 16-bit PCM audio. */
typedef struct cap_avi cap_avi;

cap_avi *cap_avi_new(const CHAR16 *path, UINTN w, UINTN h, UINTN fps,
                     UINTN audio_rate, UINTN audio_channels);
int      cap_avi_video(cap_avi *a, const UINT8 *jpeg, UINTN len);
int      cap_avi_video_dup(cap_avi *a, UINTN count);
int      cap_avi_audio(cap_avi *a, const INT16 *pcm, UINTN frames);
UINTN    cap_avi_frames(const cap_avi *a);
UINT64   cap_avi_bytes(const cap_avi *a);
int      cap_avi_failed(const cap_avi *a);
EFI_STATUS cap_avi_close(cap_avi *a);

static inline UINTN cap_bucket_of(UINT32 p) {
    return ((((p >> 16) & 0xF8) << 7) | (((p >> 8) & 0xF8) << 2)
            | ((p & 0xF8) >> 3));
}

#endif
