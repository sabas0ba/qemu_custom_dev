/*
 * demo - draws a fixed test scene through the renderer protocol.
 *
 * The scene is also what tests/e2e.sh verifies pixel-by-pixel, so keep the
 * drawing commands and the expected values in that script in sync.
 *
 * Usage:
 *   demo --unix PATH            (local testing against renderd --unix)
 *   demo --vsock CID PORT       (from a guest; host renderd is CID 2)
 *   demo --tcp ADDR PORT        (dev/CI; from a guest on user-mode
 *                                networking the host is 10.0.2.2)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render_client.h"

int main(int argc, char **argv)
{
    struct render_client rc;
    int r;

    if (argc == 3 && strcmp(argv[1], "--unix") == 0) {
        r = rc_connect_unix(&rc, argv[2]);
    } else if (argc == 4 && strcmp(argv[1], "--vsock") == 0) {
        r = rc_connect_vsock(&rc, (uint32_t)strtoul(argv[2], NULL, 10),
                             (uint32_t)strtoul(argv[3], NULL, 10));
    } else if (argc == 4 && strcmp(argv[1], "--tcp") == 0) {
        r = rc_connect_tcp(&rc, argv[2],
                           (uint16_t)strtoul(argv[3], NULL, 10));
    } else {
        fprintf(stderr,
                "usage: demo --unix PATH | demo --vsock CID PORT"
                " | demo --tcp ADDR PORT\n");
        return 2;
    }
    if (r < 0) {
        fprintf(stderr, "demo: connect failed\n");
        return 1;
    }

    if (rc_hello(&rc) < 0 ||
        rc_create_surface(&rc, 320, 240) < 0 ||
        rc_clear(&rc, 0x102030ff) < 0 ||
        rc_fill_rect(&rc, 40, 40, 80, 60, 0xff0000ff) < 0 ||
        rc_fill_rect(&rc, 160, 120, 100, 80, 0x00ff00ff) < 0 ||
        rc_present(&rc, 1) < 0 ||
        rc_goodbye(&rc) < 0) {
        fprintf(stderr, "demo: protocol error\n");
        rc_close(&rc);
        return 1;
    }
    rc_close(&rc);
    printf("demo: frame 1 presented\n");
    return 0;
}
