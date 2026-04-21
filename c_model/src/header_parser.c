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
        /* Phase 13: Pq=0 -> 8b entries, Pq=1 -> 16b entries (MSB first).
           Cross-check Pq <= (P==8?0:1) is deferred to decode (DQT may precede SOF). */
        if (pq > 1) { *err |= JPEG_ERR_UNSUP_PREC; return -1; }
        if (pq == 0) {
            for (int i = 0; i < 64; i++) {
                uint8_t v;
                if (bs_read_byte(bs, &v)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
                info->qtables[tq].q[ZZ_NATURAL[i]] = v;
            }
            remain -= 64;
        } else {
            for (int i = 0; i < 64; i++) {
                uint16_t v;
                if (bs_read_u16(bs, &v)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
                info->qtables[tq].q[ZZ_NATURAL[i]] = v;
            }
            remain -= 128;
        }
        info->qtables[tq].loaded = 1;
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

/* Shared SOF parser. Caller supplies the allowed precision range; common SOFs
 * pass narrow ranges while SOF3 (lossless) opens the full ISO H.1 range.
 *   SOF0 : (8, 8)   baseline
 *   SOF1 : accept 8 and 12 (extended sequential)
 *   SOF2 : (8, 8)   progressive, Phase 3 scope
 *   SOF3 : (2, 16)  lossless, ISO H.1.2.1 P∈{2..16}
 */
static int parse_sof_common(bitstream_t *bs, jpeg_info_t *info, uint32_t *err,
                            uint8_t p_min, uint8_t p_max) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    uint8_t p;
    if (bs_read_byte(bs, &p)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    /* SOF1 accepts only 8 and 12 (not 9-11); express via [8,12] range plus
     * an explicit check for the sequential DCT case. The simplest unified
     * predicate: p must be in [p_min, p_max]; SOF1 callers pass (8, 12) and
     * we additionally reject 9-11 here when the range is ≤ 12 and p_min == 8
     * AND the sequential gap is forbidden by caller convention — handled by
     * SOF1 passing p_max=12 and SOF3 passing p_max=16.  P in {9,10,11} is OK
     * for SOF3 but not SOF0/1/2. */
    if (p < p_min || p > p_max) { *err |= JPEG_ERR_UNSUP_PREC; return -1; }
    /* SOF0/1/2 carve-out: DCT-family SOFs only allow 8 and 12; reject the
     * {9,10,11} gap that lossless allows. */
    if (p_max == 12 && p != 8 && p != 12) {
        *err |= JPEG_ERR_UNSUP_PREC;
        return -1;
    }
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
    /* Phase 12: accept Nf ∈ {1, 3, 4} (grayscale / YCbCr / CMYK) */
    if (nf != 1 && nf != 3 && nf != 4) { *err |= JPEG_ERR_UNSUP_CHROMA; return -1; }
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

    if (nf == 3) {
        /* Chroma 必须 H=V=1；Y 允许 H=V=2 (4:2:0) 或 H=V=1 (4:4:4) */
        if (info->components[1].h_samp != 1 || info->components[1].v_samp != 1 ||
            info->components[2].h_samp != 1 || info->components[2].v_samp != 1) {
            *err |= JPEG_ERR_UNSUP_CHROMA;
            return -1;
        }
        if (info->components[0].h_samp == 2 && info->components[0].v_samp == 2) {
            info->chroma_mode = CHROMA_420;
            info->mcu_cols = (info->width  + 15) / 16;
            info->mcu_rows = (info->height + 15) / 16;
        } else if (info->components[0].h_samp == 1 && info->components[0].v_samp == 1) {
            /* Phase 9: 4:4:4 — MCU 8x8 */
            info->chroma_mode = CHROMA_444;
            info->mcu_cols = (info->width  + 7) / 8;
            info->mcu_rows = (info->height + 7) / 8;
        } else if (info->components[0].h_samp == 2 && info->components[0].v_samp == 1) {
            /* Phase 10: 4:2:2 — MCU 16x8 (2 Y blocks horizontally + Cb + Cr) */
            info->chroma_mode = CHROMA_422;
            info->mcu_cols = (info->width  + 15) / 16;
            info->mcu_rows = (info->height + 7) / 8;
        } else if (info->components[0].h_samp == 1 && info->components[0].v_samp == 2) {
            /* Phase 11a: 4:4:0 — MCU 8x16 (2 Y blocks vertically + Cb + Cr) */
            info->chroma_mode = CHROMA_440;
            info->mcu_cols = (info->width  + 7)  / 8;
            info->mcu_rows = (info->height + 15) / 16;
        } else if (info->components[0].h_samp == 4 && info->components[0].v_samp == 1) {
            /* Phase 11b: 4:1:1 — MCU 32x8 (4 Y blocks horizontally + Cb + Cr) */
            info->chroma_mode = CHROMA_411;
            info->mcu_cols = (info->width  + 31) / 32;
            info->mcu_rows = (info->height + 7)  / 8;
        } else {
            *err |= JPEG_ERR_UNSUP_CHROMA;
            return -1;
        }
    } else if (nf == 4) {
        /* Phase 12: CMYK — all components must be H=V=1 (1x1x1x1) */
        for (int i = 0; i < 4; i++) {
            if (info->components[i].h_samp != 1 || info->components[i].v_samp != 1) {
                *err |= JPEG_ERR_UNSUP_CHROMA;
                return -1;
            }
        }
        info->chroma_mode = CHROMA_CMYK;
        info->mcu_cols = (info->width  + 7) / 8;
        info->mcu_rows = (info->height + 7) / 8;
    } else {
        /* Phase 8: grayscale requires H=V=1; MCU=8x8 */
        if (info->components[0].h_samp != 1 || info->components[0].v_samp != 1) {
            *err |= JPEG_ERR_UNSUP_CHROMA;
            return -1;
        }
        info->chroma_mode = CHROMA_GRAY;
        info->mcu_cols = (info->width  + 7) / 8;
        info->mcu_rows = (info->height + 7) / 8;
    }
    return 0;
}

static int parse_sof0(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    info->sof_type = 0;
    return parse_sof_common(bs, info, err, 8, 8);
}

static int parse_sof1(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    info->sof_type = 1;
    return parse_sof_common(bs, info, err, 8, 12);
}

/* Phase 16b: SOF2 progressive. P=8 only for now (progressive + P=12 is rare
 * and out of Phase 16 scope). Component layout / chroma rules identical to
 * SOF0; scan-level progressive parameters are validated in parse_sos. */
static int parse_sof2(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    info->sof_type = 2;
    return parse_sof_common(bs, info, err, 8, 8);
}

/* Phase 25a/27: SOF3 lossless. Component layout identical to SOF0/1/2. Precision
 * P ∈ {2..16} per ISO H.1.2.1. Phase 25a/b/c gated the decode path to P=8 only;
 * Phase 27 extends decode to all 15 precisions. Scan-level params (Ss=predictor
 * 1-7, Se=0, Ah=0, Al=point-transform) validated in parse_sos. */
static int parse_sof3(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    info->sof_type = 3;
    return parse_sof_common(bs, info, err, 2, 16);
}

static int parse_sos(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    uint8_t ns;
    if (bs_read_byte(bs, &ns)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    /* Phase 8: baseline requires Ns == num_components.
     * Phase 17a: SOF2 progressive allows non-interleaved AC scans (Ns=1).
     *            ISO G.1.1.1 also permits Ns <= 4 for interleaved scans. */
    if (info->sof_type != 2 && ns != info->num_components) {
        *err |= JPEG_ERR_UNSUP_CHROMA; return -1;
    }
    if (info->sof_type == 2) {
        if (ns == 0 || ns > info->num_components) {
            *err |= JPEG_ERR_UNSUP_CHROMA; return -1;
        }
    }
    info->scan_num_comps = ns;

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
        info->scan_comp_idx[i] = (uint8_t)comp_idx;
    }

    uint8_t ss, se, ah_al;
    if (bs_read_byte(bs, &ss)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_byte(bs, &se)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_byte(bs, &ah_al)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }

    /* Phase 16a: capture scan params unconditionally so downstream (and future
     * progressive decode) can inspect them. */
    info->scan_ss = ss;
    info->scan_se = se;
    info->scan_ah = ah_al >> 4;
    info->scan_al = ah_al & 0x0F;

    /* Baseline (SOF0/SOF1) requires the full-block scan. SOF2 is validated
     * inside decode_progressive. SOF3 (Phase 25a) uses Ss as predictor Ps
     * ∈ {1..7}, Se=0, Ah=0, Al=Pt (0..15). */
    if (info->sof_type == 3) {
        if (ss < 1 || ss > 7 || se != 0 || (ah_al >> 4) != 0) {
            *err |= JPEG_ERR_UNSUP_SOF;
            return -1;
        }
    } else if (info->sof_type != 2) {
        if (ss != 0 || se != 63 || ah_al != 0) {
            *err |= JPEG_ERR_UNSUP_SOF;
            return -1;
        }
    }
    return 0;
}

static int parse_dri(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    if (bs_read_u16(bs, &info->dri)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    /* Phase 7: DRI > 0 现在合法；restart_count 由 decoder.c 管理 */
    return 0;
}

static int skip_segment(bitstream_t *bs, uint32_t *err) {
    uint16_t len;
    if (bs_read_u16(bs, &len)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
    bs_skip(bs, len - 2);
    return 0;
}

/* Phase 17a: between-scan marker loop. Consumes DHT/DQT/DRI/COM/APPn and stops
 * at EOI (returns 1) or SOS (returns 0, info->scan_* updated). */
int jpeg_parse_between_scans(bitstream_t *bs, jpeg_info_t *info, uint32_t *err) {
    /* If a marker is already pending in the bitstream (entropy decoder hit
     * 0xFF followed by a non-stuff byte), consume it first. */
    uint8_t first_marker = 0;
    int have_marker = 0;
    if (bs->marker_pending) {
        first_marker = bs->last_marker;
        bs->marker_pending = 0;
        bs->last_marker    = 0;
        have_marker = 1;
    } else {
        bs_align_to_byte(bs);
    }

    while (1) {
        uint8_t marker;
        if (have_marker) {
            marker = first_marker;
            have_marker = 0;
        } else {
            uint8_t b;
            if (bs_read_byte(bs, &b)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
            while (b != 0xFF) {
                if (bs_read_byte(bs, &b)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
            }
            while (b == 0xFF) {
                if (bs_read_byte(bs, &b)) { *err |= JPEG_ERR_STREAM_TRUNC; return -1; }
            }
            if (b == 0x00) continue; /* stuffed byte in rare mis-aligned path */
            marker = b;
        }

        switch (marker) {
            case MARKER_EOI:
                return 1;
            case MARKER_SOS:
                if (parse_sos(bs, info, err)) return -1;
                return 0;
            case MARKER_DHT:
                if (parse_dht(bs, info, err)) return -1;
                break;
            case MARKER_DQT:
                if (parse_dqt(bs, info, err)) return -1;
                break;
            case MARKER_DRI:
                if (parse_dri(bs, info, err)) return -1;
                break;
            case MARKER_COM:
                if (skip_segment(bs, err)) return -1;
                break;
            default:
                if (marker >= MARKER_APP0 && marker <= MARKER_APP15) {
                    if (skip_segment(bs, err)) return -1;
                } else {
                    /* SOF/RST/etc between scans is not allowed. */
                    *err |= JPEG_ERR_BAD_MARKER;
                    return -1;
                }
                break;
        }
    }
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
            case MARKER_SOF1:
                if (parse_sof1(bs, info, err)) return -1;
                got_sof = 1;
                break;
            case MARKER_SOF2:
                if (parse_sof2(bs, info, err)) return -1;
                got_sof = 1;
                break;
            case MARKER_SOF3:
                if (parse_sof3(bs, info, err)) return -1;
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
                } else if (marker >= MARKER_SOF5 && marker <= MARKER_SOF15) {
                    /* Phase 16b: SOF2 (progressive) now handled above.
                     * Phase 25a: SOF3 (lossless) now handled above.
                     * SOF5..SOF15 (hierarchical / arith / lossless+arith) remain
                     * unsupported — Waves 4/5/6. */
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
