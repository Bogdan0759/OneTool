#include <ranal/ranal.h>
#include <srapi/srapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    int counter;
    float volume;
    int dark_mode;
    char username[64];
    ranal_widget_t *root;
    ranal_widget_t *card;
    ranal_widget_t *title;
} demo_state_t;
static demo_state_t g_state;
static void on_increment(ranal_widget_t *w, void *user) {
    demo_state_t *s = user;
    s->counter++;
    (void)w;
}
static void on_reset(ranal_widget_t *w, void *user) {
    demo_state_t *s = user;
    s->counter = 0;
    (void)w;
}
static void on_quit(ranal_widget_t *w, void *user) {
    (void)w; (void)user;
    ranal_request_close();
}
static void on_volume(ranal_widget_t *w, float v, void *user) {
    demo_state_t *s = user;
    s->volume = v;
    (void)w;
}
static void on_dark(ranal_widget_t *w, int v, void *user) {
    demo_state_t *s = user;
    s->dark_mode = v;
    ranal_set_theme(v ? &ranal_theme_light : &ranal_theme_dark);
    (void)w;
}
int main(int argc, char *argv[]) {
    const char *record_path = NULL;
    uint32_t record_fps = 30;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("ranal_demo - demo of the ranal GUI library on top of srapi\n");
            printf("usage: %s [--record file.srvid] [--record-fps n] --debug\n", argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--record-fps") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (end == NULL || *end != '\0' || v == 0 || v > 240) {
                fprintf(stderr, "bad --record-fps\n");
                return 1;
            }
            record_fps = (uint32_t)v;
        } else if (strcmp(argv[i], "--debug") == 0) {
            srapi = 1;
        }
    }
    ranal_window_desc_t desc = { .width = 0, .height = 0, .title = "ranal demo" };
    if (ranal_init(&desc) != RANAL_OK) {
        fprintf(stderr, "ranal_demo: %s\n", ranal_last_error());
        return 1;
    }
    if (record_path != NULL) {
        if (ranal_record_start(record_path, record_fps) != RANAL_OK) {
            fprintf(stderr, "ranal_demo: %s\n", ranal_last_error());
            ranal_shutdown();
            return 1;
        }
        fprintf(stderr, "ranal_demo: recording -> %s @ %u fps\n", record_path, record_fps);
    }
    memset(&g_state, 0, sizeof(g_state));
    g_state.volume = 0.5f;
    snprintf(g_state.username, sizeof(g_state.username), "guest");
    ranal_widget_t *root = ranal_root();
    ranal_widget_t *card = ranal_panel(root);
    ranal_set_layout(card, RANAL_LAYOUT_VERT);
    ranal_set_padding(card, 24);
    ranal_set_spacing(card, 10);
    ranal_set_fill_parent(card, 1, 1);
    ranal_widget_t *title = ranal_label(card, "test");
    ranal_set_role(title, RANAL_ROLE_ACCENT);
    g_state.root = root;
    g_state.card = card;
    g_state.title = title;
    ranal_label(card, "");
    ranal_widget_t *row = ranal_panel(card);
    ranal_set_layout(row, RANAL_LAYOUT_HORIZ);
    ranal_set_spacing(row, 8);
    ranal_set_padding(row, 0);
    ranal_set_auto_size(row, 1, 1);

    ranal_widget_t *btn_inc = ranal_button(row, "inc");
    ranal_on_click(btn_inc, on_increment, &g_state);
    ranal_widget_t *btn_reset = ranal_button(row, "reset");
    ranal_on_click(btn_reset, on_reset, &g_state);
    ranal_widget_t *counter_label = ranal_label(row, "0");

    ranal_label(card, "");
    ranal_label(card, "volume");
    ranal_widget_t *vol = ranal_slider(card, 0.0f, 1.0f, g_state.volume);
    ranal_on_slide(vol, on_volume, &g_state);
    ranal_widget_t *vol_label = ranal_label(card, "0.50");

    ranal_label(card, "");
    ranal_widget_t *cb = ranal_checkbox(card, "white mode", 0);
    ranal_on_toggle(cb, on_dark, &g_state);

    ranal_label(card, "");
    ranal_label(card, "textbox");
    ranal_widget_t *tb = ranal_textbox(card, g_state.username, sizeof(g_state.username));

    ranal_label(card, "");
    ranal_widget_t *btn_quit = ranal_button(card, "Quit");
    ranal_on_click(btn_quit, on_quit, NULL);

    printf("ranal_demo: window %dx%d\n", ranal_window_width(), ranal_window_height());
    printf("controls: Esc=quit, click to interact, type to edit text\n");

    while (!ranal_frame()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "counter: %d", g_state.counter);
        ranal_set_text(counter_label, buf);
        snprintf(buf, sizeof(buf), "%.2f", g_state.volume);
        ranal_set_text(vol_label, buf);
    }
    printf("ranal_demo: final state counter=%d volume=%.2f dark=%d user=%s\n",
           g_state.counter, g_state.volume, g_state.dark_mode, g_state.username);
    ranal_shutdown();
    return 0;
}
