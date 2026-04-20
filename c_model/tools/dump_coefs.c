/*
 * Dump DCT coefficients for each block in component 0 (Y), natural order,
 * for comparison with the custom decoder. Uses libjpeg's jpeg_read_coefficients
 * which returns post-decode integer coefficients BEFORE dequantization and IDCT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.jpg [component=0]\n", argv[0]); return 2; }
    int want_comp = (argc >= 3) ? atoi(argv[2]) : 0;

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 2; }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    jvirt_barray_ptr *coefs = jpeg_read_coefficients(&cinfo);
    if (!coefs) { fprintf(stderr, "read_coefficients failed\n"); return 3; }

    jpeg_component_info *comp = &cinfo.comp_info[want_comp];
    JDIMENSION by;
    int blk_idx = 0;
    for (by = 0; by < comp->height_in_blocks; by++) {
        JBLOCKARRAY row = (*cinfo.mem->access_virt_barray)(
            (j_common_ptr)&cinfo, coefs[want_comp], by, 1, FALSE);
        for (JDIMENSION bx = 0; bx < comp->width_in_blocks; bx++) {
            JCOEFPTR blk = row[0][bx];
            printf("blk=%d by=%u bx=%u coef:", blk_idx, by, bx);
            for (int i = 0; i < 64; i++) printf(" %d", blk[i]);
            printf("\n");
            blk_idx++;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 0;
}
