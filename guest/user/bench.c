/*
 * bench - request/response latency and throughput over any transport.
 *
 * Exists to answer the Phase 3 question the project set out to measure:
 * how polling the shared-memory ring (Phase 2, --shm-pci) compares with
 * doorbell interrupts (Phase 3, --doorbell). Both use the same rings and
 * the same messages, so the difference is purely in how the peer learns
 * that a message is waiting.
 *
 * The measured operation is a FILL_RECT of an empty rectangle on a 1x1
 * surface: a full request/response round trip through the protocol with
 * as little renderer work attached to it as possible.
 *
 * With --scene it also draws and presents the standard test scene before
 * disconnecting, so a single run can both produce a verifiable frame and
 * report timings — which is what tests/vm-e2e.sh needs: the guest holds
 * exactly one shared region, and one session per region avoids having to
 * synchronise a reset between two of them.
 *
 * Usage: bench <transport> [--iters N] [--warmup N] [--scene]
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

int main(int argc, char **argv)
{
    struct render_client rc;
    long iters = 2000;
    long warmup = 200;
    int scene = 0;
    double *samples;
    double total = 0;
    int used = rc_connect_argv(&rc, argc - 1, argv + 1);

    if (used < 0) {
        fprintf(stderr, "usage: bench %s [--iters N] [--warmup N]"
                " [--scene]\n", rc_transport_usage());
        return argc > 1 ? 1 : 2;
    }
    for (int i = used + 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            iters = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
            warmup = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--scene") == 0)
            scene = 1;
        else {
            fprintf(stderr, "bench: unknown argument %s\n", argv[i]);
            rc_close(&rc);
            return 2;
        }
    }
    if (iters < 1) {
        fprintf(stderr, "bench: --iters must be positive\n");
        rc_close(&rc);
        return 2;
    }

    samples = calloc((size_t)iters, sizeof(*samples));
    if (!samples) {
        rc_close(&rc);
        return 1;
    }

    if (rc_hello(&rc) < 0 || rc_create_surface(&rc, 1, 1) < 0) {
        fprintf(stderr, "bench: handshake failed\n");
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
        total += samples[i];
    }
    if (scene && rc_draw_demo_scene(&rc, 1) < 0) {
        fprintf(stderr, "bench: drawing the scene failed\n");
        goto fail;
    }
    rc_goodbye(&rc);
    rc_close(&rc);

    qsort(samples, (size_t)iters, sizeof(*samples), cmp_double);
    printf("iters       %ld\n", iters);
    printf("mean_us     %.2f\n", total / (double)iters);
    printf("p50_us      %.2f\n", samples[iters / 2]);
    printf("p99_us      %.2f\n", samples[(iters * 99) / 100]);
    printf("min_us      %.2f\n", samples[0]);
    printf("max_us      %.2f\n", samples[iters - 1]);
    printf("roundtrips_per_sec %.0f\n", 1e6 / (total / (double)iters));
    free(samples);
    return 0;

fail:
    free(samples);
    rc_close(&rc);
    return 1;
}
