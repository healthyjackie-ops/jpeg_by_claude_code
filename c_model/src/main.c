#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    size_t un = (size_t)n;
    uint8_t *buf = (uint8_t*)malloc(un);
    if (fread(buf, 1, un, f) != un) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = un;
    return buf;
}

static int write_ppm(const char *path, const jpeg_decoded_t *d) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", d->width, d->height);
    for (uint32_t i = 0; i < (uint32_t)d->width * d->height; i++) {
        int y  = d->y_plane[i];
        int cb = (int)d->cb_plane[i] - 128;
        int cr = (int)d->cr_plane[i] - 128;
        int r = y + ((91881 * cr + 32768) >> 16);
        int g = y - ((22554 * cb + 46802 * cr + 32768) >> 16);
        int b = y + ((116130 * cb + 32768) >> 16);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        uint8_t rgb[3] = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static int write_yuv420(const char *path, const jpeg_decoded_t *d) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(d->y_plane, 1, (size_t)d->width * d->height, f);
    fwrite(d->cb_plane_420, 1, (size_t)(d->width >> 1) * (d->height >> 1), f);
    fwrite(d->cr_plane_420, 1, (size_t)(d->width >> 1) * (d->height >> 1), f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <in.jpg> [out.ppm] [out.yuv]\n", argv[0]);
        return 1;
    }

    size_t size;
    uint8_t *data = read_file(argv[1], &size);
    if (!data) { perror("read"); return 1; }

    jpeg_decoded_t dec;
    int rc = jpeg_decode(data, size, &dec);
    if (rc != 0) {
        fprintf(stderr, "decode error: 0x%X\n", dec.err);
        free(data);
        return 2;
    }

    fprintf(stderr, "decoded %ux%u\n", dec.width, dec.height);

    if (argc >= 3) write_ppm(argv[2], &dec);
    if (argc >= 4) write_yuv420(argv[3], &dec);

    jpeg_free(&dec);
    free(data);
    return 0;
}
