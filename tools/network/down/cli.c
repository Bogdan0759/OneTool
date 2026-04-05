#include "down.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    down_request_t req;
    FILE *out = stdout;
    int rc;

    rc = down_parse_cli(argc, argv, &req);
    if (rc == 2) {
        down_request_free(&req);
        return 0;
    }
    if (rc != 0) {
        down_request_free(&req);
        return 1;
    }

    if (req.output_path != NULL) {
        out = fopen(req.output_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "down: fopen(%s) failed: %s\n", req.output_path, strerror(errno));
            down_request_free(&req);
            return 1;
        }
    }

    rc = down_run(&req, out);

    if (out != stdout) {
        fclose(out);
    }
    down_request_free(&req);
    return rc;
}
