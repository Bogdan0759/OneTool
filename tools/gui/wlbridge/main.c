#include "wayland_server.h"
#include <sprot/sprot.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(const char *prog) {
    printf("wlbridge - SWM wayland to sprot proxy server\n");
    printf("usage: %s [--display=wayland-0] [--socket=/path/to/swm.sock] [--debug=/path/to/debug.log]\n", prog);
}

int main(int argc, char *argv[]) {
    const char *display_name = "wayland-0";
    const char *swm_socket = SPROT_DEFAULT_SOCKET;
    const char *debug_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--display=", 10) == 0) {
            display_name = argv[i] + 10;
        } else if (strncmp(argv[i], "--socket=", 9) == 0) {
            swm_socket = argv[i] + 9;
        } else if (strncmp(argv[i], "--debug=", 8) == 0) {
            debug_file = argv[i] + 8;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("[wlbridge] display: %s\n", display_name);
    printf("[wlbridge] socket: %s\n", swm_socket);
    if (debug_file) {
        printf("[wlbridge] debug file: %s\n", debug_file);
    }

    return wayland_server_run(display_name, swm_socket, debug_file);
}
