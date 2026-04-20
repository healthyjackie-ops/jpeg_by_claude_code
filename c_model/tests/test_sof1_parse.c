/* Phase 13a.2: verify header parser accepts SOF1 (0xFFC1), P=12 and Pq=1 DQT.
 * Full P=12 decode is Phase 13a.3 — here we only check parsed metadata and
 * that jpeg_decode() refuses cleanly with JPEG_ERR_UNSUP_PREC. */
#include "decoder.h"
#include "header_parser.h"
#include "jpeg_types.h"
#include "bitstream.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *len_out = (size_t)n;
    return buf;
}

static int check_one(const char *path) {
    size_t n = 0;
    uint8_t *buf = read_file(path, &n);
    if (!buf) { fprintf(stderr, "  cannot read %s\n", path); return -1; }

    bitstream_t bs;
    bs_init(&bs, buf, n);
    jpeg_info_t info;
    uint32_t err = 0;
    int rc = jpeg_parse_headers(&bs, &info, &err);
    if (rc != 0) {
        fprintf(stderr, "  parse failed %s err=0x%X\n", path, err);
        free(buf);
        return -1;
    }
    assert(info.precision == 12);
    assert(info.num_components == 1 || info.num_components == 3);
    assert(info.qtables[info.components[0].qt_id].loaded);

    /* jpeg_decode must refuse cleanly with JPEG_ERR_UNSUP_PREC (13a.2 guard). */
    jpeg_decoded_t d;
    int drc = jpeg_decode(buf, n, &d);
    assert(drc != 0);
    assert(d.err & JPEG_ERR_UNSUP_PREC);
    jpeg_free(&d);
    free(buf);
    return 0;
}

int main(void) {
    printf("== test_sof1_parse ==\n");
    const char *dir = "../verification/vectors/phase13";
    DIR *dp = opendir(dir);
    if (!dp) {
        fprintf(stderr, "SKIP: %s not present (run tools/gen_phase13.py first)\n", dir);
        return 0;
    }
    struct dirent *e;
    int n = 0, ok = 0;
    while ((e = readdir(dp))) {
        const char *name = e->d_name;
        size_t l = strlen(name);
        if (l < 4 || strcmp(name + l - 4, ".jpg") != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        ++n;
        if (check_one(path) == 0) ++ok;
    }
    closedir(dp);
    printf("  parsed %d/%d Phase 13 vectors with P=12 OK\n", ok, n);
    assert(n > 0 && ok == n);
    return 0;
}
