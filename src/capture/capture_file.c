/* capture_file.c - ESP file helpers for saving captures */
#include "capture_internal.h"

#ifndef CAP_NO_IO

EFI_STATUS cap_ensure_dir(const CHAR16 *path) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return EFI_DEVICE_ERROR;
    EFI_FILE_PROTOCOL *d = NULL;
    EFI_STATUS st = root->Open(root, &d, (CHAR16*)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !d) {
        st = root->Open(root, &d, (CHAR16*)path,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                        EFI_FILE_DIRECTORY);
    }
    if (d) d->Close(d);
    root->Close(root);
    return EFI_ERROR(st) ? st : EFI_SUCCESS;
}

EFI_STATUS cap_save_file(const CHAR16 *path, const UINT8 *data, UINTN size) {
    if (!path || (!data && size)) return EFI_INVALID_PARAMETER;
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return EFI_DEVICE_ERROR;

    /* CREATE opens an existing file without truncating it. Two captures in the
     * same second - or any second at all on a box whose RTC does not answer,
     * where every name comes out as _000000 - would then leave the tail of the
     * larger old file glued to the new one. Delete the namesake first. */
    EFI_FILE_PROTOCOL *old = NULL;
    if (!EFI_ERROR(root->Open(root, &old, (CHAR16*)path,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && old)
        old->Delete(old);

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, (CHAR16*)path,
                               EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                               0);
    if (EFI_ERROR(st) || !f) {
        root->Close(root);
        return EFI_DEVICE_ERROR;
    }
    UINTN w = size ? size : 0;
    st = f->Write(f, &w, (void*)data);
    if (!EFI_ERROR(st)) st = f->Flush(f);
    if (EFI_ERROR(st)) st = EFI_DEVICE_ERROR;
    f->Close(f);
    root->Close(root);
    return st;
}

void cap_timestamp_name(CHAR16 *out, UINTN cap, const CHAR16 *base,
                        const CHAR16 *ext) {
    UINTN ho = 0, mi = 0, se = 0;
    EFI_TIME t;
    if (!EFI_ERROR(RT->GetTime(&t, NULL))) {
        ho = t.Hour; mi = t.Minute; se = t.Second;
    }
    if (ho > 99) ho = 0;
    if (mi > 99) mi = 0;
    if (se > 99) se = 0;
    UINTN n = 0;
    for (UINTN i = 0; base && base[i] && n + 1 < cap; i++) out[n++] = base[i];
    if (n + 1 < cap) out[n++] = L'_';
    CHAR16 digs[6];
    digs[0] = (CHAR16)(L'0' + ho / 10); digs[1] = (CHAR16)(L'0' + ho % 10);
    digs[2] = (CHAR16)(L'0' + mi / 10); digs[3] = (CHAR16)(L'0' + mi % 10);
    digs[4] = (CHAR16)(L'0' + se / 10); digs[5] = (CHAR16)(L'0' + se % 10);
    for (UINTN i = 0; i < 6 && n + 1 < cap; i++) out[n++] = digs[i];
    if (ext) for (UINTN i = 0; ext[i] && n + 1 < cap; i++) out[n++] = ext[i];
    out[n < cap ? n : cap - 1] = 0;
}

/* --- streaming sink -------------------------------------------------------
 * A boot recording can run for as long as the menu is up, so it is written
 * straight through to the ESP instead of being assembled in RAM. Small writes
 * to FAT are expensive, so everything lands in a staging buffer first and is
 * flushed in large blocks.
 */

#define CAP_FILE_BUF (256u * 1024u)

struct cap_file {
    EFI_FILE_PROTOCOL *fh;
    UINT8  *buf;
    UINTN   len;
    UINT64  pos;        /* bytes handed to us, buffered or not */
    UINT64  flushed;    /* file offset of the first buffered byte */
    int     err;
};

static int cap_file_flush(cap_file *f) {
    if (f->err) return 0;
    if (!f->len) return 1;
    UINTN n = f->len;
    EFI_STATUS st = f->fh->Write(f->fh, &n, f->buf);
    if (EFI_ERROR(st) || n != f->len) { f->err = 1; return 0; }
    f->flushed += f->len;
    f->len = 0;
    return 1;
}

cap_file *cap_file_create(const CHAR16 *path) {
    if (!path) return NULL;
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return NULL;

    /* CREATE opens an existing file without truncating it, which would leave
     * a stale tail behind. Delete any namesake first. */
    EFI_FILE_PROTOCOL *old = NULL;
    if (!EFI_ERROR(root->Open(root, &old, (CHAR16*)path,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && old)
        old->Delete(old);

    EFI_FILE_PROTOCOL *fh = NULL;
    EFI_STATUS st = root->Open(root, &fh, (CHAR16*)path,
                               EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
                               EFI_FILE_MODE_WRITE, 0);
    root->Close(root);
    if (EFI_ERROR(st) || !fh) return NULL;

    cap_file *f = efi_allocate_pool(sizeof(cap_file));
    if (!f) { fh->Close(fh); return NULL; }
    memset(f, 0, sizeof(*f));
    f->fh  = fh;
    f->buf = efi_allocate_pool(CAP_FILE_BUF);
    if (!f->buf) {
        fh->Close(fh);
        efi_free_pool(f);
        return NULL;
    }
    return f;
}

int cap_file_write(cap_file *f, const void *data, UINTN len) {
    if (!f || f->err) return 0;
    const UINT8 *p = (const UINT8*)data;
    while (len) {
        UINTN room = CAP_FILE_BUF - f->len;
        UINTN n = len < room ? len : room;
        CopyMem(f->buf + f->len, (void*)p, n);
        f->len += n;
        f->pos += n;
        p      += n;
        len    -= n;
        if (f->len == CAP_FILE_BUF && !cap_file_flush(f)) return 0;
    }
    return 1;
}

/* Patching a header field means leaving the append position, so flush first
 * and put the cursor back afterwards. */
int cap_file_write_at(cap_file *f, UINT64 pos, const void *data, UINTN len) {
    if (!f || f->err) return 0;
    if (!cap_file_flush(f)) return 0;

    UINT64 back = f->flushed;
    if (EFI_ERROR(f->fh->SetPosition(f->fh, pos))) { f->err = 1; return 0; }
    UINTN n = len;
    EFI_STATUS st = f->fh->Write(f->fh, &n, (void*)data);
    if (EFI_ERROR(st) || n != len) { f->err = 1; return 0; }
    if (EFI_ERROR(f->fh->SetPosition(f->fh, back))) { f->err = 1; return 0; }
    return 1;
}

UINT64 cap_file_pos(const cap_file *f) { return f ? f->pos : 0; }

void cap_file_close(cap_file *f) {
    if (!f) return;
    cap_file_flush(f);
    if (f->fh) {
        f->fh->Flush(f->fh);
        f->fh->Close(f->fh);
    }
    if (f->buf) efi_free_pool(f->buf);
    efi_free_pool(f);
}

#endif
