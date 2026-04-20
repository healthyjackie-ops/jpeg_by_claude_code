#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <setjmp.h>

struct err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void err_exit(j_common_ptr cinfo) {
    struct err_mgr *e = (struct err_mgr*)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(e->jb, 1);
}

typedef struct {
    uint32_t width;
    uint32_t height;
    int num_components;   /* 1=grayscale, 3=YCbCr */
    int chroma_mode;      /* 0=gray, 1=420, 2=444 */
    uint32_t cb_width;    /* native chroma plane width */
    uint32_t cb_height;   /* native chroma plane height */
    uint8_t *y;
    uint8_t *cb;
    uint8_t *cr;
} libjpeg_ycc_t;

static int libjpeg_decode_ycc(const uint8_t *data, size_t size, libjpeg_ycc_t *out) {
    struct jpeg_decompress_struct cinfo;
    struct err_mgr jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(out, 0, sizeof(*out));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = err_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); return -1; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char*)data, size);
    jpeg_read_header(&cinfo, TRUE);

    int is_gray = (cinfo.num_components == 1);
    int is_444  = 0;
    int is_422  = 0;
    if (!is_gray && cinfo.num_components == 3) {
        int h0 = cinfo.comp_info[0].h_samp_factor;
        int v0 = cinfo.comp_info[0].v_samp_factor;
        int h1 = cinfo.comp_info[1].h_samp_factor;
        int v1 = cinfo.comp_info[1].v_samp_factor;
        int h2 = cinfo.comp_info[2].h_samp_factor;
        int v2 = cinfo.comp_info[2].v_samp_factor;
        is_444 = (h0 == 1 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1);
        is_422 = (h0 == 2 && v0 == 1 && h1 == 1 && v1 == 1 && h2 == 1 && v2 == 1);
    }
    cinfo.raw_data_out = TRUE;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.dct_method = JDCT_ISLOW;
    cinfo.out_color_space = is_gray ? JCS_GRAYSCALE : JCS_YCbCr;

    if (!jpeg_start_decompress(&cinfo)) { jpeg_destroy_decompress(&cinfo); return -1; }

    uint32_t W = cinfo.output_width;
    uint32_t H = cinfo.output_height;
    out->width = W;
    out->height = H;
    out->num_components = is_gray ? 1 : 3;
    out->chroma_mode = is_gray ? 0 : (is_444 ? 2 : (is_422 ? 3 : 1));

    if (is_gray) {
        /* Phase 8: grayscale — raw_data_out fills 8 rows of Y per call.
           MCU-aligned pad buffer, crop later. */
        uint32_t Wp = ((W + 7) / 8) * 8;
        uint32_t Hp = ((H + 7) / 8) * 8;
        uint8_t *y_pad = (uint8_t*)calloc((size_t)Wp * Hp, 1);
        JSAMPROW y_rowptrs[8];
        JSAMPARRAY arrays[1] = { y_rowptrs };
        while (cinfo.output_scanline < H) {
            uint32_t base_y = cinfo.output_scanline;
            for (uint32_t i = 0; i < 8; i++) {
                y_rowptrs[i] = y_pad + (size_t)(base_y + i) * Wp;
            }
            (void)jpeg_read_raw_data(&cinfo, arrays, 8);
        }
        out->y = (uint8_t*)calloc((size_t)W * H, 1);
        for (uint32_t r = 0; r < H; r++) {
            memcpy(out->y + (size_t)r * W, y_pad + (size_t)r * Wp, W);
        }
        free(y_pad);
    } else if (is_444) {
        /* Phase 9: 4:4:4 — 1 MCU = 8 rows, all 3 components at full-res.
           jpeg_read_raw_data returns 8 rows Y, 8 rows Cb, 8 rows Cr per call. */
        uint32_t Wp  = ((W + 7) / 8) * 8;
        uint32_t Hp  = ((H + 7) / 8) * 8;
        uint8_t *y_pad  = (uint8_t*)calloc((size_t)Wp * Hp, 1);
        uint8_t *cb_pad = (uint8_t*)calloc((size_t)Wp * Hp, 1);
        uint8_t *cr_pad = (uint8_t*)calloc((size_t)Wp * Hp, 1);

        JSAMPROW y_rowptrs[8];
        JSAMPROW cb_rowptrs[8];
        JSAMPROW cr_rowptrs[8];
        JSAMPARRAY arrays[3] = { y_rowptrs, cb_rowptrs, cr_rowptrs };

        while (cinfo.output_scanline < H) {
            uint32_t base_y = cinfo.output_scanline;
            for (uint32_t i = 0; i < 8; i++) {
                y_rowptrs[i]  = y_pad  + (size_t)(base_y + i) * Wp;
                cb_rowptrs[i] = cb_pad + (size_t)(base_y + i) * Wp;
                cr_rowptrs[i] = cr_pad + (size_t)(base_y + i) * Wp;
            }
            (void)jpeg_read_raw_data(&cinfo, arrays, 8);
        }

        /* Crop to W×H */
        out->y  = (uint8_t*)calloc((size_t)W * H, 1);
        out->cb = (uint8_t*)calloc((size_t)W * H, 1);
        out->cr = (uint8_t*)calloc((size_t)W * H, 1);
        out->cb_width  = W;
        out->cb_height = H;
        for (uint32_t r = 0; r < H; r++) {
            memcpy(out->y  + (size_t)r * W, y_pad  + (size_t)r * Wp, W);
            memcpy(out->cb + (size_t)r * W, cb_pad + (size_t)r * Wp, W);
            memcpy(out->cr + (size_t)r * W, cr_pad + (size_t)r * Wp, W);
        }
        free(y_pad); free(cb_pad); free(cr_pad);
    } else if (is_422) {
        /* Phase 10: 4:2:2 — MCU 16x8. jpeg_read_raw_data returns 8 rows
           Y at Wp, 8 rows Cb/Cr at Wp/2 per call. */
        uint32_t Wp = ((W + 15) / 16) * 16;
        uint32_t Hp = ((H + 7) / 8) * 8;
        uint32_t CWp = Wp / 2;
        uint8_t *y_pad  = (uint8_t*)calloc((size_t)Wp  * Hp, 1);
        uint8_t *cb_pad = (uint8_t*)calloc((size_t)CWp * Hp, 1);
        uint8_t *cr_pad = (uint8_t*)calloc((size_t)CWp * Hp, 1);

        JSAMPROW y_rowptrs[8];
        JSAMPROW cb_rowptrs[8];
        JSAMPROW cr_rowptrs[8];
        JSAMPARRAY arrays[3] = { y_rowptrs, cb_rowptrs, cr_rowptrs };

        while (cinfo.output_scanline < H) {
            uint32_t base_y = cinfo.output_scanline;
            for (uint32_t i = 0; i < 8; i++) {
                y_rowptrs[i]  = y_pad  + (size_t)(base_y + i) * Wp;
                cb_rowptrs[i] = cb_pad + (size_t)(base_y + i) * CWp;
                cr_rowptrs[i] = cr_pad + (size_t)(base_y + i) * CWp;
            }
            (void)jpeg_read_raw_data(&cinfo, arrays, 8);
        }

        /* Crop to W×H Y and (W/2)×H chroma */
        out->y  = (uint8_t*)calloc((size_t)W * H, 1);
        out->cb = (uint8_t*)calloc((size_t)(W/2) * H, 1);
        out->cr = (uint8_t*)calloc((size_t)(W/2) * H, 1);
        out->cb_width  = W/2;
        out->cb_height = H;
        for (uint32_t r = 0; r < H; r++) {
            memcpy(out->y  + (size_t)r * W,     y_pad  + (size_t)r * Wp,  W);
            memcpy(out->cb + (size_t)r * (W/2), cb_pad + (size_t)r * CWp, W/2);
            memcpy(out->cr + (size_t)r * (W/2), cr_pad + (size_t)r * CWp, W/2);
        }
        free(y_pad); free(cb_pad); free(cr_pad);
    } else {
        /* 4:2:0: raw_data_out 要求 MCU 对齐的行缓冲。每次调用 jpeg_read_raw_data
           会填 16 行 Y / 8 行 Cb / 8 行 Cr，哪怕 H 不是 16 的倍数。
           用 padded 临时缓冲，之后 crop 到 W×H。 */
        uint32_t Wp = ((W + 15) / 16) * 16;
        uint32_t Hp = ((H + 15) / 16) * 16;
        uint32_t CWp = Wp / 2;
        uint32_t CHp = Hp / 2;
        uint8_t *y_pad  = (uint8_t*)calloc((size_t)Wp  * Hp,  1);
        uint8_t *cb_pad = (uint8_t*)calloc((size_t)CWp * CHp, 1);
        uint8_t *cr_pad = (uint8_t*)calloc((size_t)CWp * CHp, 1);

        JSAMPROW y_rowptrs[16];
        JSAMPROW cb_rowptrs[8];
        JSAMPROW cr_rowptrs[8];
        JSAMPARRAY arrays[3] = { y_rowptrs, cb_rowptrs, cr_rowptrs };

        while (cinfo.output_scanline < H) {
            uint32_t base_y = cinfo.output_scanline;
            for (uint32_t i = 0; i < 16; i++) {
                y_rowptrs[i]  = y_pad + (size_t)(base_y + i) * Wp;
            }
            uint32_t base_c = base_y >> 1;
            for (uint32_t i = 0; i < 8; i++) {
                cb_rowptrs[i] = cb_pad + (size_t)(base_c + i) * CWp;
                cr_rowptrs[i] = cr_pad + (size_t)(base_c + i) * CWp;
            }
            (void)jpeg_read_raw_data(&cinfo, arrays, 16);
        }

        /* Crop padded buffers to actual W×H / (W/2)×(H/2). */
        out->y  = (uint8_t*)calloc((size_t)W * H, 1);
        out->cb = (uint8_t*)calloc((size_t)(W/2) * (H/2), 1);
        out->cr = (uint8_t*)calloc((size_t)(W/2) * (H/2), 1);
        out->cb_width  = W/2;
        out->cb_height = H/2;
        for (uint32_t r = 0; r < H; r++) {
            memcpy(out->y + (size_t)r * W, y_pad + (size_t)r * Wp, W);
        }
        for (uint32_t r = 0; r < H/2; r++) {
            memcpy(out->cb + (size_t)r * (W/2), cb_pad + (size_t)r * CWp, W/2);
            memcpy(out->cr + (size_t)r * (W/2), cr_pad + (size_t)r * CWp, W/2);
        }
        free(y_pad); free(cb_pad); free(cr_pad);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

static void libjpeg_free(libjpeg_ycc_t *o) {
    free(o->y); free(o->cb); free(o->cr);
    memset(o, 0, sizeof(*o));
}

static uint8_t *read_file(const char *p, size_t *s) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    size_t un = (size_t)n;
    uint8_t *b = (uint8_t*)malloc(un);
    if (fread(b, 1, un, f) != un) { free(b); fclose(f); return NULL; }
    fclose(f); *s = un; return b;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <jpg1> [jpg2 ...]\n", argv[0]);
        return 1;
    }
    int total = 0, pass = 0, fail = 0, skip = 0;
    int worst_diff_y = 0, worst_diff_c = 0;

    for (int a = 1; a < argc; a++) {
        total++;
        size_t sz; uint8_t *buf = read_file(argv[a], &sz);
        if (!buf) { fprintf(stderr, "[SKIP] %s: read fail\n", argv[a]); skip++; continue; }

        libjpeg_ycc_t gold;
        if (libjpeg_decode_ycc(buf, sz, &gold)) {
            fprintf(stderr, "[SKIP] %s: libjpeg reject\n", argv[a]);
            skip++; free(buf); continue;
        }

        jpeg_decoded_t ours;
        int rc = jpeg_decode(buf, sz, &ours);
        if (rc != 0) {
            fprintf(stderr, "[FAIL] %s: our decode err=0x%X\n", argv[a], ours.err);
            fail++;
            libjpeg_free(&gold); free(buf); continue;
        }

        if (ours.width != gold.width || ours.height != gold.height) {
            fprintf(stderr, "[FAIL] %s: size %ux%u vs gold %ux%u\n",
                    argv[a], ours.width, ours.height, gold.width, gold.height);
            fail++;
            jpeg_free(&ours); libjpeg_free(&gold); free(buf); continue;
        }

        int dy_max = 0, dc_max = 0;
        size_t npix = (size_t)gold.width * gold.height;
        for (size_t i = 0; i < npix; i++) {
            int d = (int)ours.y_plane[i] - (int)gold.y[i];
            if (d < 0) d = -d;
            if (d > dy_max) dy_max = d;
        }
        if (gold.chroma_mode == 1) {
            /* 4:2:0: compare pre-upsample half-res planes */
            size_t ncpix = (size_t)gold.cb_width * gold.cb_height;
            for (size_t i = 0; i < ncpix; i++) {
                int d1 = (int)ours.cb_plane_420[i] - (int)gold.cb[i];
                int d2 = (int)ours.cr_plane_420[i] - (int)gold.cr[i];
                if (d1 < 0) d1 = -d1;
                if (d2 < 0) d2 = -d2;
                if (d1 > dc_max) dc_max = d1;
                if (d2 > dc_max) dc_max = d2;
            }
        } else if (gold.chroma_mode == 2) {
            /* Phase 9: 4:4:4 — compare full-res cb_plane / cr_plane */
            size_t ncpix = (size_t)gold.cb_width * gold.cb_height;
            for (size_t i = 0; i < ncpix; i++) {
                int d1 = (int)ours.cb_plane[i] - (int)gold.cb[i];
                int d2 = (int)ours.cr_plane[i] - (int)gold.cr[i];
                if (d1 < 0) d1 = -d1;
                if (d2 < 0) d2 = -d2;
                if (d1 > dc_max) dc_max = d1;
                if (d2 > dc_max) dc_max = d2;
            }
        } else if (gold.chroma_mode == 3) {
            /* Phase 10: 4:2:2 — compare pre-upsample (W/2)×H planes */
            size_t ncpix = (size_t)gold.cb_width * gold.cb_height;
            for (size_t i = 0; i < ncpix; i++) {
                int d1 = (int)ours.cb_plane_422[i] - (int)gold.cb[i];
                int d2 = (int)ours.cr_plane_422[i] - (int)gold.cr[i];
                if (d1 < 0) d1 = -d1;
                if (d2 < 0) d2 = -d2;
                if (d1 > dc_max) dc_max = d1;
                if (d2 > dc_max) dc_max = d2;
            }
        }

        const char *tag = (gold.chroma_mode == 0) ? " [GRAY]" :
                          (gold.chroma_mode == 2) ? " [444]"  :
                          (gold.chroma_mode == 3) ? " [422]"  : "";
        if (dy_max == 0 && dc_max == 0) {
            printf("[PASS] %s %ux%u exact%s\n", argv[a], gold.width, gold.height, tag);
            pass++;
        } else {
            printf("[FAIL] %s %ux%u maxDiff Y=%d C=%d%s\n",
                   argv[a], gold.width, gold.height, dy_max, dc_max, tag);
            fail++;
        }
        if (dy_max > worst_diff_y) worst_diff_y = dy_max;
        if (dc_max > worst_diff_c) worst_diff_c = dc_max;

        jpeg_free(&ours);
        libjpeg_free(&gold);
        free(buf);
    }
    printf("\n=== SUMMARY ===\n");
    printf("Total: %d  Pass: %d  Fail: %d  Skip: %d\n", total, pass, fail, skip);
    printf("Worst diff: Y=%d  C=%d\n", worst_diff_y, worst_diff_c);
    return (fail == 0) ? 0 : 3;
}
