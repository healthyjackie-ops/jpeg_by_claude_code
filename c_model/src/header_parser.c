#include "header_parser.h"
#include <string.h>
#include <stdio.h>

static const uint8_t ZZ_NATURAL[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

void jpeg_build_huffman_tables(htable_t *h) {
    int huffsize[257];
    uint16_t huffcode[257];

    int p = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < h->bits[l]; i++) {
            huffsize[p++] = l;
        }
    }
    huffsize[p] = 0;
    int lastp = p;

    uint16_t code = 0;
    int si = huffsize[0];
    p = 0;
    while (huffsize[p]) {
        while (huffsize[p] == si) {
            huffcode[p++] = code++;
        }
        if (huffsize[p] == 0) break;
        do {
            code <<= 1;
            si++;
        } while (huffsize[p] != si);
    }

    p = 0;
    for (int l = 1; l <= 16; l++) {
        if (h->bits[l] == 0) {
            h->maxcode[l] = -1;
        } else {
            h->valptr[l]  = (uint8_t)p;
            h->mincode[l] = huffcode[p];
            p += h->bits[l];
            h->maxcode[l] = huffcode[p - 1];
        }
    }
    h->maxcode[17] = 0xFFFFF;
    (void)lastp;
    h->loaded = 1;
}

static int parse_dqt(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    int remain = len - 2;
    while (remain > 0) {
        uint8_t pq_tq;
        if (bs_read_byte(bs, &pq_tq)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        int pq = pq_tq >> 4;
        int tq = pq_tq & 0xF;
        remain -= 1;
        if (tq >= 4) { *err |= JPEG_ERR_BAD_MARKER; return -1; }
        if (pq != 0) { *err |= JPEG_ERR_UNSUP_PREC; return -1; }
        for (int i = 0; i < 64; i++) {
            uint8_t v;
            if (bs_read_byte(bs, &v)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
            info->qtables[tq].q[ZZ_NATURAL[i]] = v;
        }
        info->qtables[tq].loaded = 1;
        remain -= 64;
    }
    return 0;
}

static int parse_dht(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    int remain = len - 2;
    while (remain > 0) {
        uint8_t tc_th;
        if (bs_read_byte(bs, &tc_th)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        int tc = tc_th >> 4;
        int th = tc_th & 0xF;
        remain -= 1;
        if (tc > 1 || th >= 4) { *err |= JPEG_ERR_BAD_MARKER; return -1; }
        htable_t *h = (tc == 0) ? &info->htables_dc[th] : &info->htables_ac[th];
        memset(h, 0, sizeof(*h));
        int total = 0;
        for (int i = 1; i <= 16; i++) {
            uint8_t v;
            if (bs_read_byte(bs, &v)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
            h->bits[i] = v;
            total += v;
        }
        remain -= 16;
        if (total > 256) { *err |= JPEG_ERR_BAD_MARKER; return -1; }
        for (int i = 0; i < total; i++) {
            uint8_t v;
            if (bs_read_byte(bs, &v)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
            h->huffval[i] = v;
        }
        remain -= total;
        jpeg_build_huffman_tables(h);
    }
    return 0;
}

static int parse_sof0(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    uint8_t p;
    if (bs_read_byte(bs, &p)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (p != 8) { *err |= JPEG_ERR_UNSUP_PREC; return -1; }
    info->precision = p;
    if (bs_read_u16(bs, &info->height)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_u16(bs, &info->width))  { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }

    if (info->width == 0 || info->height == 0 ||
        info->width > JPEG_MAX_WIDTH || info->height > JPEG_MAX_HEIGHT) {
        *err |= JPEG_ERR_SIZE_OOR;
        return -1;
    }
    /* Phase 6: 非 16 对齐尺寸允许；decoder 自己做 padding + crop */

    uint8_t nf;
    if (bs_read_byte(bs, &nf)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (nf != 3) { *err |= JPEG_ERR_UNSUP_CHROMA; return -1; }
    info->num_components = nf;

    for (int i = 0; i < nf; i++) {
        uint8_t id, hv, tq;
        if (bs_read_byte(bs, &id)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        if (bs_read_byte(bs, &hv)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        if (bs_read_byte(bs, &tq)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        info->components[i].id = id;
        info->components[i].h_samp = hv >> 4;
        info->components[i].v_samp = hv & 0xF;
        info->components[i].qt_id = tq;
    }

    if (info->components[0].h_samp != 2 || info->components[0].v_samp != 2 ||
        info->components[1].h_samp != 1 || info->components[1].v_samp != 1 ||
        info->components[2].h_samp != 1 || info->components[2].v_samp != 1) {
        *err |= JPEG_ERR_UNSUP_CHROMA;
        return -1;
    }

    /* Phase 6: 向上取整支持非对齐尺寸 */
    info->mcu_cols = (info->width  + 15) / 16;
    info->mcu_rows = (info->height + 15) / 16;
    return 0;
}

static int parse_sos(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    uint8_t ns;
    if (bs_read_byte(bs, &ns)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (ns != 3) { *err |= JPEG_ERR_UNSUP_CHROMA; return -1; }

    for (int i = 0; i < ns; i++) {
        uint8_t cs, tdta;
        if (bs_read_byte(bs, &cs)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        if (bs_read_byte(bs, &tdta)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        int comp_idx = -1;
        for (int j = 0; j < info->num_components; j++) {
            if (info->components[j].id == cs) { comp_idx = j; break; }
        }
        if (comp_idx < 0) { *err |= JPEG_ERR_BAD_MARKER; return -1; }
        info->components[comp_idx].td = tdta >> 4;
        info->components[comp_idx].ta = tdta & 0xF;
    }

    uint8_t ss, se, ah_al;
    if (bs_read_byte(bs, &ss)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_byte(bs, &se)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_byte(bs, &ah_al)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }

    if (ss != 0 || se != 63 || ah_al != 0) {
        *err |= JPEG_ERR_UNSUP_SOF;
        return -1;
    }
    return 0;
}

static int parse_dri(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_u16(bs, &info->dri)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (info->dri != 0) { *err |= JPEG_ERR_DRI_NONZERO; return -1; }
    return 0;
}

static int skip_segment(bitstream_t *bs, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    bs_skip(bs, len - 2);
    return 0;
}

int jpeg_parse_headers(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    memset(info, 0, sizeof(*info));

    uint8_t b;
    if (bs_read_byte(bs, &b) || b != 0xFF) { *err |= JPEG_ERR_BAD_MARKER; return -1; }
    if (bs_read_byte(bs, &b) || b != MARKER_SOI) { *err |= JPEG_ERR_BAD_MARKER; return -1; }

    int got_sof = 0;
    while (1) {
        if (bs_read_byte(bs, &b)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        while (b != 0xFF) {
            if (bs_read_byte(bs, &b)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        }
        while (b == 0xFF) {
            if (bs_read_byte(bs, &b)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
        }

        uint8_t marker = b;
        switch (marker) {
            case MARKER_SOF0:
                if (parse_sof0(bs, info, err)) return -1;
                got_sof = 1;
                break;
            case MARKER_DQT:
                if (parse_dqt(bs, info, err)) return -1;
                break;
            case MARKER_DHT:
                if (parse_dht(bs, info, err)) return -1;
                break;
            case MARKER_DRI:
                if (parse_dri(bs, info, err)) return -1;
                break;
            case MARKER_SOS:
                if (!got_sof) { *err |= JPEG_ERR_BAD_MARKER; return -1; }
                if (parse_sos(bs, info, err)) return -1;
                return 0;
            case MARKER_EOI:
                *err |= JPEG_ERR_STREAM_TRUNC;
                return -1;
            case MARKER_COM:
                if (skip_segment(bs, err)) return -1;
                break;
            default:
                if (marker >= MARKER_APP0 && marker <= MARKER_APP15) {
                    if (skip_segment(bs, err)) return -1;
                } else if (marker == MARKER_SOF1 || (marker >= MARKER_SOF2 && marker <= MARKER_SOF3) ||
                           (marker >= MARKER_SOF5 && marker <= MARKER_SOF15)) {
                    *err |= JPEG_ERR_UNSUP_SOF;
                    return -1;
                } else {
                    *err |= JPEG_ERR_BAD_MARKER;
                    return -1;
                }
                break;
        }
    }
}
