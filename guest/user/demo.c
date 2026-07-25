/*
 * demo - draws a fixed test scene through the renderer protocol.
 *
 * The scene is also what tests/e2e.sh verifies pixel-by-pixel, so keep the
 * drawing commands and the expected values in that script in sync.
 *
 * Usage: demo <transport>, where <transport> is one of the specs listed
 * by rc_transport_usage(), e.g.
 *   demo --unix PATH            (local testing against renderd --unix)
 *   demo --vsock CID PORT       (from a guest; host renderd is CID 2)
 *   demo --tcp ADDR PORT        (dev/CI; from a guest on user-mode
 *                                networking the host is 10.0.2.2)
 *   demo --shm-file PATH        (Phase 2 shm transport over a plain file)
 *   demo --shm-pci              (Phase 2, from a guest, polled)
 *   demo --doorbell             (Phase 3, from a guest, interrupt-driven)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render_client.h"

int main(int argc, char **argv)
{
    struct render_client rc;
    int used = rc_connect_argv(&rc, argc - 1, argv + 1);

    if (used < 0) {
        fprintf(stderr, "usage: demo %s\n", rc_transport_usage());
        return argc > 1 ? 1 : 2;
    }
    if (used + 1 != argc) {
        fprintf(stderr, "demo: unexpected extra arguments\n");
        rc_close(&rc);
        return 2;
    }

    if (rc_hello(&rc) < 0 ||
        rc_draw_demo_scene(&rc, 1) < 0 ||
        rc_goodbye(&rc) < 0) {
        fprintf(stderr, "demo: protocol error\n");
        rc_close(&rc);
        return 1;
    }
    rc_close(&rc);
    printf("demo: frame 1 presented\n");
    return 0;
}
