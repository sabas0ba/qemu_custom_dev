/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bench - request/response latency and pixel throughput over any transport.
 *
 * Exists to answer the two questions the project set out to measure.
 *
 * Latency (always): how polling the shared-memory ring (Phase 2,
 * --shm-pci) compares with doorbell interrupts (Phase 3, --doorbell).
 * Both use the same rings and the same messages, so the difference is
 * purely in how the peer learns that a message is waiting. The measured
 * operation is a FILL_RECT of an empty rectangle on a 1x1 surface: a full
 * round trip with as little renderer work attached as possible. Every
 * transport can run it, so the numbers stay comparable across all of them.
 *
 * Throughput (--blit, shared-memory transports only): what the shared
 * region actually buys. Pixels are staged in shared memory and composited
 * with BLIT (protocol v0.2), so a whole frame moves without ever passing
 * through the command ring.
 *
 * --scene / --blit-scene additionally draw the project's standard test
 * scene before disconnecting, through FILL_RECT and through BLIT
 * respectively. Either lets one run produce both a verifiable frame and
 * timings, which is what tests/vm-e2e.sh needs: the guest holds exactly
 * one shared region, and keeping to one session per region avoids having
 * to synchronise a reset between two of them.
 *
 * Usage:
 *   bench <transport> [--iters N] [--warmup N] [--blit [W H]]
 *                     [--scene | --blit-scene]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "render_client.h"

static double now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Print the distribution of `iters` samples under a name prefix, and
 * return the mean. Sorts samples in place. */
static double report(const char *prefix, double *samples, long iters)
{
    double total = 0;

    for (long i = 0; i < iters; i++)
        total += samples[i];
    qsort(samples, (size_t)iters, sizeof(*samples), cmp_double);
    printf("%smean_us     %.2f\n", prefix, total / (double)iters);
    printf("%sp50_us      %.2f\n", prefix, samples[iters / 2]);
    printf("%sp99_us      %.2f\n", prefix, samples[(iters * 99) / 100]);
    printf("%smin_us      %.2f\n", prefix, samples[0]);
    printf("%smax_us      %.2f\n", prefix, samples[iters - 1]);
    return total / (double)iters;
}

int main(int argc, char **argv)
{
    struct render_client rc;
    long iters = 2000;
    long warmup = 200;
    int scene = 0, blit_scene = 0, blit = 0;
    long bw = 640, bh = 480;
    double *samples;
    int used = rc_connect_argv(&rc, argc - 1, argv + 1);

    if (used < 0) {
        fprintf(stderr, "usage: bench %s [--iters N] [--warmup N]"
                " [--blit [W H]] [--scene | --blit-scene]\n",
                rc_transport_usage());
        return argc > 1 ? 1 : 2;
    }
    for (int i = used + 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--scene") == 0) {
            scene = 1;
        } else if (strcmp(argv[i], "--blit-scene") == 0) {
            blit_scene = 1;
        } else if (strcmp(argv[i], "--blit") == 0) {
            blit = 1;
            /* optional W H right after the flag */
            if (i + 2 < argc && argv[i + 1][0] != '-') {
                bw = strtol(argv[i + 1], NULL, 10);
                bh = strtol(argv[i + 2], NULL, 10);
                i += 2;
            }
        } else {
            fprintf(stderr, "bench: unknown argument %s\n", argv[i]);
            rc_close(&rc);
            return 2;
        }
    }
    if (iters < 1 || warmup < 0 || bw < 1 || bh < 1) {
        fprintf(stderr, "bench: bad iteration count or dimensions\n");
        rc_close(&rc);
        return 2;
    }

    samples = calloc((size_t)iters, sizeof(*samples));
    if (!samples) {
        rc_close(&rc);
        return 1;
    }
    if (rc_hello(&rc) < 0) {
        fprintf(stderr, "bench: handshake failed\n");
        goto fail;
    }
    printf("iters       %ld\n", iters);

    /* --- round-trip latency, on every transport --- */
    if (rc_create_surface(&rc, 1, 1) < 0) {
        fprintf(stderr, "bench: cannot create the surface\n");
        goto fail;
    }
    for (long i = 0; i < warmup; i++)
        if (rc_fill_rect(&rc, 0, 0, 0, 0, 0) < 0) {
            fprintf(stderr, "bench: warmup failed\n");
            goto fail;
        }
    for (long i = 0; i < iters; i++) {
        double t0 = now_us();

        if (rc_fill_rect(&rc, 0, 0, 0, 0, 0) < 0) {
            fprintf(stderr, "bench: request %ld failed\n", i);
            goto fail;
        }
        samples[i] = now_us() - t0;
    }
    printf("roundtrips_per_sec %.0f\n", 1e6 / report("", samples, iters));

    /* --- pixel throughput, shared-memory transports only --- */
    if (blit) {
        size_t staging_size = 0;
        uint8_t *staging = rc_staging(&rc, &staging_size);
        size_t frame_bytes = (size_t)bw * (size_t)bh * 4u;
        double mean;

        if (!staging || staging_size < frame_bytes) {
            fprintf(stderr, "bench: --blit needs a shared-memory transport"
                    " with room for a %ldx%ld frame\n", bw, bh);
            goto fail;
        }
        if (rc_create_surface(&rc, (uint32_t)bw, (uint32_t)bh) < 0) {
            fprintf(stderr, "bench: cannot create the blit surface\n");
            goto fail;
        }
        /* A non-uniform pattern, so nothing can collapse the copy. */
        for (size_t i = 0; i < frame_bytes; i++)
            staging[i] = (uint8_t)i;

        for (long i = 0; i < warmup; i++)
            if (rc_blit(&rc, 0, (uint32_t)bw * 4u, 0, 0,
                        (uint32_t)bw, (uint32_t)bh) < 0) {
                fprintf(stderr, "bench: blit warmup failed\n");
                goto fail;
            }
        for (long i = 0; i < iters; i++) {
            double t0 = now_us();

            if (rc_blit(&rc, 0, (uint32_t)bw * 4u, 0, 0,
                        (uint32_t)bw, (uint32_t)bh) < 0) {
                fprintf(stderr, "bench: blit %ld failed\n", i);
                goto fail;
            }
            samples[i] = now_us() - t0;
        }
        mean = report("blit_", samples, iters);
        printf("blit_frame_bytes %zu\n", frame_bytes);
        printf("blit_frames_per_sec %.1f\n", 1e6 / mean);
        printf("blit_throughput_mib_s %.1f\n",
               (double)frame_bytes / (1024.0 * 1024.0) * (1e6 / mean));
    }

    if (blit_scene) {
        if (rc_draw_blit_scene(&rc, 1) < 0) {
            fprintf(stderr, "bench: drawing the blit scene failed\n");
            goto fail;
        }
    } else if (scene && rc_draw_demo_scene(&rc, 1) < 0) {
        fprintf(stderr, "bench: drawing the scene failed\n");
        goto fail;
    }

    rc_goodbye(&rc);
    rc_close(&rc);
    free(samples);
    return 0;

fail:
    free(samples);
    rc_close(&rc);
    return 1;
}
