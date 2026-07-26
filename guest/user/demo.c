/* SPDX-License-Identifier: GPL-2.0-only */
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
 *   demo --virtio               (Phase 4, from a guest, over a virtqueue)
 *
 * With a trailing --blit the same scene is composited from pixels staged
 * in shared memory (protocol v0.2) rather than drawn with FILL_RECT; the
 * resulting frame is identical. Shared-memory transports only.
 *
 * With --rich it draws the v0.3 scene instead: a filled triangle, a line
 * and a half-transparent rectangle composited over them. Works on every
 * transport.
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
        fprintf(stderr, "usage: demo %s [--blit | --rich]\n",
                rc_transport_usage());
        return argc > 1 ? 1 : 2;
    }
    int blit = 0, rich = 0;

    for (int i = used + 1; i < argc; i++) {
        if (strcmp(argv[i], "--blit") == 0) {
            blit = 1;
        } else if (strcmp(argv[i], "--rich") == 0) {
            rich = 1;
        } else {
            fprintf(stderr, "demo: unexpected argument %s\n", argv[i]);
            rc_close(&rc);
            return 2;
        }
    }

    if (rc_hello(&rc) < 0 ||
        (rich ? rc_draw_rich_scene(&rc, 1)
              : blit ? rc_draw_blit_scene(&rc, 1)
                     : rc_draw_demo_scene(&rc, 1)) < 0 ||
        rc_goodbye(&rc) < 0) {
        fprintf(stderr, "demo: protocol error\n");
        rc_close(&rc);
        return 1;
    }
    rc_close(&rc);
    printf("demo: frame 1 presented\n");
    return 0;
}
