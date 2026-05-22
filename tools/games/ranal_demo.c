#include <ranal/ranal.h>
#include <stdio.h>
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
    (void)argc; (void)argv;
    ranal_window_desc_t desc = { .width = 0, .height = 0, .title = "ranal demo" };
    if (ranal_init(&desc) != RANAL_OK) {
        fprintf(stderr, "ranal_demo: %s\n", ranal_last_error());
        return 1;
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
    ranal_widget_t *title = ranal_label(card, "ranal - gui on top of srapi");
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

    ranal_widget_t *btn_inc = ranal_button(row, "+ increment");
    ranal_on_click(btn_inc, on_increment, &g_state);
    ranal_widget_t *btn_reset = ranal_button(row, "reset");
    ranal_on_click(btn_reset, on_reset, &g_state);
    ranal_widget_t *counter_label = ranal_label(row, "counter: 0");

    ranal_label(card, "");
    ranal_label(card, "volume");
    ranal_widget_t *vol = ranal_slider(card, 0.0f, 1.0f, g_state.volume);
    ranal_on_slide(vol, on_volume, &g_state);
    ranal_widget_t *vol_label = ranal_label(card, "0.50");

    ranal_label(card, "");
    ranal_widget_t *cb = ranal_checkbox(card, "enable dark mode", 0);
    ranal_on_toggle(cb, on_dark, &g_state);

    ranal_label(card, "");
    ranal_label(card, "username (click to edit)");
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
