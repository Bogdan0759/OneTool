/*
 * browser - OneTool GUI web browser entry point.
 *
 * usage: browser [--swm [WxH]] [--title TITLE] [URL]
 *
 *   --swm [WxH]    run as a sprot/swm client (windowed)
 *   --title TITLE  window title (default: "browser")
 *   URL            initial URL to fetch (otherwise opens about:home)
 */
#include "app.h"
#include "ui/input.h"

#include <ranal/ranal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(const char *prog) {
    printf("browser - OneTool GUI web browser\n");
    printf("usage: %s [--swm [WxH]] [--title TITLE] [URL]\n", prog);
    printf("\n");
    printf("  --swm [WxH]    run as a sprot/swm client (windowed)\n");
    printf("  --title TITLE  window title (default: browser)\n");
    printf("  (no flag)      run standalone using ranal/srapi backend\n");
    printf("  URL            initial URL to navigate to (optional)\n");
    printf("\n");
    printf("In-app keys:\n");
    printf("  Ctrl+L / /     focus URL bar\n");
    printf("  Enter          submit URL / follow focused link\n");
    printf("  Tab            move link focus\n");
    printf("  Up/Down PgUp/PgDn Space Home/End  scroll\n");
    printf("  Alt+Left       back\n");
    printf("  F5 / Ctrl+R    reload\n");
    printf("  Esc            quit (or unfocus URL bar)\n");
}

int main(int argc, char *argv[]) {
    int swm_mode = 0;
    int32_t swm_w = BROWSER_DEFAULT_W;
    int32_t swm_h = BROWSER_DEFAULT_H;
    const char *swm_title = "browser";
    const char *initial_url = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(a, "--swm") == 0) {
            swm_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int w = 0, h = 0;
                if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    swm_w = w; swm_h = h; i++;
                }
            }
        } else if (strcmp(a, "--title") == 0 && i + 1 < argc) {
            swm_title = argv[++i];
        } else if (a[0] != '-') {
            initial_url = a;
        }
    }

    if (swm_mode) {
        if (ranal_init_swm(swm_title, swm_w, swm_h) != RANAL_OK) {
            fprintf(stderr, "browser: %s\n", ranal_last_error());
            return 1;
        }
    } else {
        ranal_window_desc_t desc = { .width = 0, .height = 0, .title = "browser" };
        if (ranal_init(&desc) != RANAL_OK) {
            fprintf(stderr, "browser: %s\n", ranal_last_error());
            return 1;
        }
    }

    browser_app_t app;
    if (br_app_init(&app, initial_url) != 0) {
        fprintf(stderr, "browser: init failed\n");
        ranal_shutdown();
        return 1;
    }

    br_input_install(&app);

    while (!ranal_should_close()) {
        if (br_app_frame(&app) != 0) break;
    }

    br_app_shutdown(&app);
    ranal_shutdown();
    return 0;
}
