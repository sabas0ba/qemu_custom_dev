/*
 * ppm_check - assert the color of one pixel in a binary PPM (P6) file.
 *
 * Usage: ppm_check FILE X Y RRGGBB
 * Exits 0 when the pixel matches, 1 otherwise. Used by tests/e2e.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: ppm_check FILE X Y RRGGBB\n");
        return 2;
    }
    const char *path = argv[1];
    long x = strtol(argv[2], NULL, 10);
    long y = strtol(argv[3], NULL, 10);
    unsigned long want = strtoul(argv[4], NULL, 16);

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }

    char magic[3] = { 0 };
    int w, h, maxval;
    if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxval) != 4 ||
        strcmp(magic, "P6") != 0 || maxval != 255 ||
        fgetc(f) == EOF) {
        fprintf(stderr, "ppm_check: %s: unsupported or malformed PPM\n", path);
        fclose(f);
        return 1;
    }
    if (x < 0 || y < 0 || x >= w || y >= h) {
        fprintf(stderr, "ppm_check: pixel (%ld,%ld) outside %dx%d\n", x, y, w, h);
        fclose(f);
        return 1;
    }
    if (fseek(f, (y * (long)w + x) * 3, SEEK_CUR) != 0) {
        perror("ppm_check: fseek");
        fclose(f);
        return 1;
    }
    unsigned char rgb[3];
    if (fread(rgb, 1, 3, f) != 3) {
        fprintf(stderr, "ppm_check: %s: short read\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    unsigned long got = ((unsigned long)rgb[0] << 16) |
                        ((unsigned long)rgb[1] << 8) | rgb[2];
    if (got != want) {
        fprintf(stderr, "ppm_check: (%ld,%ld) is %06lx, expected %06lx\n",
                x, y, got, want);
        return 1;
    }
    return 0;
}
