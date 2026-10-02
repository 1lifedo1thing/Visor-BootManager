/* capture_jpeg.c - baseline JPEG encoder for MJPEG recording (feature: recboot)
 *
 * Sequential baseline JPEG, YCbCr 4:2:0, standard Annex K Huffman tables.
 * Everything is integer: no FPU state to save in the pre-OS environment, and
 * the same bytes come out on x86_64 and aarch64.
 *
 * The encoder is a reusable context - a recording encodes hundreds of frames,
 * so the scratch blocks and the output buffer are allocated once and the
 * per-frame path only appends to them.
 */
#include "capture_internal.h"

/* --- tables ---------------------------------------------------------------
 * ITU T.81 Annex K: the example quantisation and Huffman tables. Every JPEG
 * decoder in the world has been tested against these.
 */

static const UINT8 jpg_q_luma[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

static const UINT8 jpg_q_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Natural-order index of each zig-zag position. */
static const UINT8 jpg_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const UINT8 jpg_dc_luma_bits[17] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0
};
static const UINT8 jpg_dc_luma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const UINT8 jpg_dc_chroma_bits[17] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0
};
static const UINT8 jpg_dc_chroma_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

static const UINT8 jpg_ac_luma_bits[17] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D
};
static const UINT8 jpg_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
    0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

static const UINT8 jpg_ac_chroma_bits[17] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77
};
static const UINT8 jpg_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
    0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
    0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

/* Derived canonical codes, shared by every encoder instance: the tables are
 * constants, so build them at most once per boot.
 * 0 = DC luma, 1 = AC luma, 2 = DC chroma, 3 = AC chroma.
 */
static UINT16 jpg_code[4][256];
static UINT8  jpg_len[4][256];
static int    jpg_tables_ready;

static void jpg_build_table(int slot, const UINT8 *bits, const UINT8 *vals,
                            UINTN nvals) {
    UINT16 code = 0;
    UINTN k = 0;
    for (UINTN l = 1; l <= 16; l++) {
        for (UINTN i = 0; i < bits[l]; i++) {
            if (k >= nvals) return;
            jpg_code[slot][vals[k]] = code;
            jpg_len[slot][vals[k]]  = (UINT8)l;
            code++;
            k++;
        }
        code = (UINT16)(code << 1);
    }
}

static void jpg_build_tables(void) {
    if (jpg_tables_ready) return;
    memset(jpg_code, 0, sizeof(jpg_code));
    memset(jpg_len,  0, sizeof(jpg_len));
    jpg_build_table(0, jpg_dc_luma_bits,   jpg_dc_luma_vals,   12);
    jpg_build_table(1, jpg_ac_luma_bits,   jpg_ac_luma_vals,   162);
    jpg_build_table(2, jpg_dc_chroma_bits, jpg_dc_chroma_vals, 12);
    jpg_build_table(3, jpg_ac_chroma_bits, jpg_ac_chroma_vals, 162);
    jpg_tables_ready = 1;
}

struct cap_jpeg {
    UINTN   w, h;
    UINTN   mcux, mcuy;
    UINT16  ql[64], qc[64];      /* natural order, quality-scaled */
    cap_buf out;
    UINT32  bitbuf;
    UINTN   bitcnt;
    int     err;
    INT32   dc[3];
    INT32   blk[6][64];          /* 4 luma + Cb + Cr for one MCU */
};

/* --- bit and byte output -------------------------------------------------- */

static void jpg_byte(cap_jpeg *j, UINT8 b) {
    if (j->err) return;
    if (!cap_buf_put(&j->out, &b, 1)) j->err = 1;
}

static void jpg_u16(cap_jpeg *j, UINT16 v) {
    jpg_byte(j, (UINT8)(v >> 8));
    jpg_byte(j, (UINT8)v);
}

static void jpg_bits(cap_jpeg *j, UINT32 code, UINTN size) {
    if (!size || j->err) return;
    j->bitbuf = (j->bitbuf << size) | (code & ((1u << size) - 1u));
    j->bitcnt += size;
    while (j->bitcnt >= 8) {
        UINT8 b = (UINT8)((j->bitbuf >> (j->bitcnt - 8)) & 0xFF);
        jpg_byte(j, b);
        /* 0xFF in entropy data is a marker prefix - stuff a zero after it. */
        if (b == 0xFF) jpg_byte(j, 0x00);
        j->bitcnt -= 8;
    }
}

static void jpg_bit_flush(cap_jpeg *j) {
    if (j->bitcnt) jpg_bits(j, 0x7F, 8 - j->bitcnt);   /* pad with 1 bits */
    j->bitbuf = 0;
    j->bitcnt = 0;
}

static void jpg_huff(cap_jpeg *j, int slot, UINT8 sym) {
    jpg_bits(j, jpg_code[slot][sym], jpg_len[slot][sym]);
}

/* --- forward DCT ----------------------------------------------------------
 * The slow-but-accurate integer DCT (Loeffler-Ligtenberg-Moschytz butterflies
 * in fixed point). Output is the true DCT scaled by 8, which the quantiser
 * below divides back out.
 */

#define JPG_CB 13                        /* fixed-point bits for constants */
#define JPG_P1 2                         /* extra bits kept between passes */
#define JPG_DESCALE(x, n) (((x) + ((INT32)1 << ((n) - 1))) >> (n))

#define JPG_F_0_298631336  2446
#define JPG_F_0_390180644  3196
#define JPG_F_0_541196100  4433
#define JPG_F_0_765366865  6270
#define JPG_F_0_899976223  7373
#define JPG_F_1_175875602  9633
#define JPG_F_1_501321110 12299
#define JPG_F_1_847759065 15137
#define JPG_F_1_961570560 16069
#define JPG_F_2_053119869 16819
#define JPG_F_2_562915447 20995
#define JPG_F_3_072711026 25172

static void jpg_fdct(INT32 *b) {
    INT32 t0, t1, t2, t3, t4, t5, t6, t7;
    INT32 t10, t11, t12, t13;
    INT32 z1, z2, z3, z4, z5;

    for (int i = 0; i < 8; i++) {
        INT32 *p = b + i * 8;
        t0 = p[0] + p[7]; t7 = p[0] - p[7];
        t1 = p[1] + p[6]; t6 = p[1] - p[6];
        t2 = p[2] + p[5]; t5 = p[2] - p[5];
        t3 = p[3] + p[4]; t4 = p[3] - p[4];

        t10 = t0 + t3; t13 = t0 - t3;
        t11 = t1 + t2; t12 = t1 - t2;

        p[0] = (t10 + t11) << JPG_P1;
        p[4] = (t10 - t11) << JPG_P1;

        z1 = (t12 + t13) * JPG_F_0_541196100;
        p[2] = JPG_DESCALE(z1 + t13 * JPG_F_0_765366865, JPG_CB - JPG_P1);
        p[6] = JPG_DESCALE(z1 - t12 * JPG_F_1_847759065, JPG_CB - JPG_P1);

        z1 = t4 + t7; z2 = t5 + t6;
        z3 = t4 + t6; z4 = t5 + t7;
        z5 = (z3 + z4) * JPG_F_1_175875602;

        t4 *= JPG_F_0_298631336; t5 *= JPG_F_2_053119869;
        t6 *= JPG_F_3_072711026; t7 *= JPG_F_1_501321110;
        z1 *= -JPG_F_0_899976223; z2 *= -JPG_F_2_562915447;
        z3 *= -JPG_F_1_961570560; z4 *= -JPG_F_0_390180644;
        z3 += z5; z4 += z5;

        p[7] = JPG_DESCALE(t4 + z1 + z3, JPG_CB - JPG_P1);
        p[5] = JPG_DESCALE(t5 + z2 + z4, JPG_CB - JPG_P1);
        p[3] = JPG_DESCALE(t6 + z2 + z3, JPG_CB - JPG_P1);
        p[1] = JPG_DESCALE(t7 + z1 + z4, JPG_CB - JPG_P1);
    }

    for (int i = 0; i < 8; i++) {
        INT32 *p = b + i;
        t0 = p[0 * 8] + p[7 * 8]; t7 = p[0 * 8] - p[7 * 8];
        t1 = p[1 * 8] + p[6 * 8]; t6 = p[1 * 8] - p[6 * 8];
        t2 = p[2 * 8] + p[5 * 8]; t5 = p[2 * 8] - p[5 * 8];
        t3 = p[3 * 8] + p[4 * 8]; t4 = p[3 * 8] - p[4 * 8];

        t10 = t0 + t3; t13 = t0 - t3;
        t11 = t1 + t2; t12 = t1 - t2;

        p[0 * 8] = JPG_DESCALE(t10 + t11, JPG_P1);
        p[4 * 8] = JPG_DESCALE(t10 - t11, JPG_P1);

        z1 = (t12 + t13) * JPG_F_0_541196100;
        p[2 * 8] = JPG_DESCALE(z1 + t13 * JPG_F_0_765366865, JPG_CB + JPG_P1);
        p[6 * 8] = JPG_DESCALE(z1 - t12 * JPG_F_1_847759065, JPG_CB + JPG_P1);

        z1 = t4 + t7; z2 = t5 + t6;
        z3 = t4 + t6; z4 = t5 + t7;
        z5 = (z3 + z4) * JPG_F_1_175875602;

        t4 *= JPG_F_0_298631336; t5 *= JPG_F_2_053119869;
        t6 *= JPG_F_3_072711026; t7 *= JPG_F_1_501321110;
        z1 *= -JPG_F_0_899976223; z2 *= -JPG_F_2_562915447;
        z3 *= -JPG_F_1_961570560; z4 *= -JPG_F_0_390180644;
        z3 += z5; z4 += z5;

        p[7 * 8] = JPG_DESCALE(t4 + z1 + z3, JPG_CB + JPG_P1);
        p[5 * 8] = JPG_DESCALE(t5 + z2 + z4, JPG_CB + JPG_P1);
        p[3 * 8] = JPG_DESCALE(t6 + z2 + z3, JPG_CB + JPG_P1);
        p[1 * 8] = JPG_DESCALE(t7 + z1 + z4, JPG_CB + JPG_P1);
    }
}

/* --- entropy coding ------------------------------------------------------- */

static UINTN jpg_category(INT32 v) {
    UINTN n = 0;
    if (v < 0) v = -v;
    while (v) { n++; v >>= 1; }
    return n;
}

static void jpg_value_bits(cap_jpeg *j, INT32 v, UINTN cat) {
    if (!cat) return;
    if (v < 0) v += ((INT32)1 << cat) - 1;   /* one's complement for negatives */
    jpg_bits(j, (UINT32)v, cat);
}

static void jpg_block(cap_jpeg *j, INT32 *blk, const UINT16 *q,
                      int dc_slot, int ac_slot, INT32 *pred) {
    jpg_fdct(blk);

    INT32 qb[64];
    for (UINTN i = 0; i < 64; i++) {
        /* fdct output carries an extra factor of 8 */
        INT32 div = (INT32)q[i] * 8;
        INT32 v = blk[i];
        if (v < 0) qb[i] = -((-v + (div >> 1)) / div);
        else       qb[i] =  (( v + (div >> 1)) / div);
    }

    INT32 diff = qb[0] - *pred;
    *pred = qb[0];
    UINTN cat = jpg_category(diff);
    jpg_huff(j, dc_slot, (UINT8)cat);
    jpg_value_bits(j, diff, cat);

    UINTN run = 0;
    for (UINTN k = 1; k < 64; k++) {
        INT32 v = qb[jpg_zigzag[k]];
        if (!v) { run++; continue; }
        while (run >= 16) {
            jpg_huff(j, ac_slot, 0xF0);      /* ZRL: sixteen zeroes */
            run -= 16;
        }
        cat = jpg_category(v);
        jpg_huff(j, ac_slot, (UINT8)((run << 4) | cat));
        jpg_value_bits(j, v, cat);
        run = 0;
    }
    if (run) jpg_huff(j, ac_slot, 0x00);     /* EOB */
}

/* --- headers -------------------------------------------------------------- */

static void jpg_quant_init(UINT16 *dst, const UINT8 *base, UINTN quality) {
    if (!quality) quality = 1;
    if (quality > 100) quality = 100;
    UINTN scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (UINTN i = 0; i < 64; i++) {
        UINTN v = ((UINTN)base[i] * scale + 50) / 100;
        if (v < 1)   v = 1;
        if (v > 255) v = 255;
        dst[i] = (UINT16)v;
    }
}

static void jpg_dqt(cap_jpeg *j, const UINT16 *q, UINT8 id) {
    jpg_u16(j, 0xFFDB);
    jpg_u16(j, 67);
    jpg_byte(j, id);                         /* 8-bit precision, table id */
    for (UINTN i = 0; i < 64; i++) jpg_byte(j, (UINT8)q[jpg_zigzag[i]]);
}

static void jpg_dht(cap_jpeg *j, UINT8 id, const UINT8 *bits,
                    const UINT8 *vals, UINTN nvals) {
    jpg_u16(j, 0xFFC4);
    jpg_u16(j, (UINT16)(2 + 1 + 16 + nvals));
    jpg_byte(j, id);
    for (UINTN i = 1; i <= 16; i++) jpg_byte(j, bits[i]);
    for (UINTN i = 0; i < nvals; i++) jpg_byte(j, vals[i]);
}

static void jpg_headers(cap_jpeg *j) {
    jpg_u16(j, 0xFFD8);                      /* SOI */

    jpg_u16(j, 0xFFE0);                      /* APP0 / JFIF */
    jpg_u16(j, 16);
    jpg_byte(j, 'J'); jpg_byte(j, 'F'); jpg_byte(j, 'I'); jpg_byte(j, 'F');
    jpg_byte(j, 0);
    jpg_byte(j, 1); jpg_byte(j, 1);          /* version 1.1 */
    jpg_byte(j, 0);                          /* no density units */
    jpg_u16(j, 1); jpg_u16(j, 1);
    jpg_byte(j, 0); jpg_byte(j, 0);          /* no thumbnail */

    jpg_dqt(j, j->ql, 0);
    jpg_dqt(j, j->qc, 1);

    jpg_u16(j, 0xFFC0);                      /* SOF0 - baseline */
    jpg_u16(j, 17);
    jpg_byte(j, 8);
    jpg_u16(j, (UINT16)j->h);
    jpg_u16(j, (UINT16)j->w);
    jpg_byte(j, 3);
    jpg_byte(j, 1); jpg_byte(j, 0x22); jpg_byte(j, 0);   /* Y,  4:2:0 */
    jpg_byte(j, 2); jpg_byte(j, 0x11); jpg_byte(j, 1);   /* Cb */
    jpg_byte(j, 3); jpg_byte(j, 0x11); jpg_byte(j, 1);   /* Cr */

    /* MJPEG frames must carry their own Huffman tables - players do not
     * remember them across frames. */
    jpg_dht(j, 0x00, jpg_dc_luma_bits,   jpg_dc_luma_vals,   12);
    jpg_dht(j, 0x10, jpg_ac_luma_bits,   jpg_ac_luma_vals,   162);
    jpg_dht(j, 0x01, jpg_dc_chroma_bits, jpg_dc_chroma_vals, 12);
    jpg_dht(j, 0x11, jpg_ac_chroma_bits, jpg_ac_chroma_vals, 162);

    jpg_u16(j, 0xFFDA);                      /* SOS */
    jpg_u16(j, 12);
    jpg_byte(j, 3);
    jpg_byte(j, 1); jpg_byte(j, 0x00);
    jpg_byte(j, 2); jpg_byte(j, 0x11);
    jpg_byte(j, 3); jpg_byte(j, 0x11);
    jpg_byte(j, 0); jpg_byte(j, 63); jpg_byte(j, 0);
}

/* --- colour conversion ----------------------------------------------------
 * ITU-R BT.601 in 16-bit fixed point. Level shift (-128) is folded in: the
 * chroma transform is already centred on zero.
 */

static void jpg_mcu_load(cap_jpeg *j, const UINT32 *px, UINTN mx, UINTN my) {
    UINTN w = j->w, h = j->h;

    for (UINTN bj = 0; bj < 2; bj++) {
        for (UINTN bi = 0; bi < 2; bi++) {
            INT32 *blk = j->blk[bj * 2 + bi];
            for (UINTN yy = 0; yy < 8; yy++) {
                UINTN sy = my * 16 + bj * 8 + yy;
                if (sy >= h) sy = h - 1;
                const UINT32 *row = px + sy * w;
                for (UINTN xx = 0; xx < 8; xx++) {
                    UINTN sx = mx * 16 + bi * 8 + xx;
                    if (sx >= w) sx = w - 1;
                    UINT32 p = row[sx];
                    INT32 r = (INT32)((p >> 16) & 0xFF);
                    INT32 g = (INT32)((p >> 8) & 0xFF);
                    INT32 b = (INT32)(p & 0xFF);
                    blk[yy * 8 + xx] =
                        ((19595 * r + 38470 * g + 7471 * b) >> 16) - 128;
                }
            }
        }
    }

    INT32 *cb = j->blk[4], *cr = j->blk[5];
    for (UINTN cy = 0; cy < 8; cy++) {
        for (UINTN cx = 0; cx < 8; cx++) {
            INT32 r = 0, g = 0, b = 0;
            for (UINTN dy = 0; dy < 2; dy++) {
                UINTN sy = my * 16 + cy * 2 + dy;
                if (sy >= h) sy = h - 1;
                const UINT32 *row = px + sy * w;
                for (UINTN dx = 0; dx < 2; dx++) {
                    UINTN sx = mx * 16 + cx * 2 + dx;
                    if (sx >= w) sx = w - 1;
                    UINT32 p = row[sx];
                    r += (INT32)((p >> 16) & 0xFF);
                    g += (INT32)((p >> 8) & 0xFF);
                    b += (INT32)(p & 0xFF);
                }
            }
            r >>= 2; g >>= 2; b >>= 2;
            cb[cy * 8 + cx] = (-11056 * r - 21712 * g + 32768 * b) >> 16;
            cr[cy * 8 + cx] = ( 32768 * r - 27440 * g -  5328 * b) >> 16;
        }
    }
}

/* --- public API ----------------------------------------------------------- */

cap_jpeg *cap_jpeg_new(UINTN w, UINTN h, UINTN quality) {
    if (!w || !h || w > 0xFFFF || h > 0xFFFF) return NULL;

    jpg_build_tables();

    cap_jpeg *j = efi_allocate_pool(sizeof(cap_jpeg));
    if (!j) return NULL;
    memset(j, 0, sizeof(*j));

    j->w = w; j->h = h;
    j->mcux = (w + 15) / 16;
    j->mcuy = (h + 15) / 16;
    jpg_quant_init(j->ql, jpg_q_luma,   quality);
    jpg_quant_init(j->qc, jpg_q_chroma, quality);

    /* One byte per pixel is a generous ceiling for 4:2:0 at sane qualities;
     * the buffer still grows on its own if a frame beats the estimate. */
    if (!cap_buf_reserve(&j->out, w * h + 4096)) {
        efi_free_pool(j);
        return NULL;
    }
    return j;
}

void cap_jpeg_free(cap_jpeg *j) {
    if (!j) return;
    cap_buf_free(&j->out);
    efi_free_pool(j);
}

UINTN cap_jpeg_width(const cap_jpeg *j)  { return j ? j->w : 0; }
UINTN cap_jpeg_height(const cap_jpeg *j) { return j ? j->h : 0; }

const UINT8 *cap_jpeg_frame(cap_jpeg *j, const UINT32 *pixels, UINTN *out_len) {
    if (!j || !pixels || !out_len) return NULL;

    j->out.len = 0;
    j->bitbuf = 0;
    j->bitcnt = 0;
    j->err = 0;
    j->dc[0] = j->dc[1] = j->dc[2] = 0;

    jpg_headers(j);

    for (UINTN my = 0; my < j->mcuy; my++) {
        for (UINTN mx = 0; mx < j->mcux; mx++) {
            jpg_mcu_load(j, pixels, mx, my);
            for (UINTN b = 0; b < 4; b++)
                jpg_block(j, j->blk[b], j->ql, 0, 1, &j->dc[0]);
            jpg_block(j, j->blk[4], j->qc, 2, 3, &j->dc[1]);
            jpg_block(j, j->blk[5], j->qc, 2, 3, &j->dc[2]);
            if (j->err) return NULL;
        }
    }

    jpg_bit_flush(j);
    jpg_u16(j, 0xFFD9);                      /* EOI */

    if (j->err) return NULL;
    *out_len = j->out.len;
    return j->out.buf;
}
