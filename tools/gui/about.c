#define _GNU_SOURCE
#include <ranal/ranal.h>
#include <srapi/srapi.h>
#include <sprot/sprot.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

extern const char onetool_version[];

#define ABOUT_W 760
#define ABOUT_H 480
#define SIDEBAR_W 158
#define LINE_MAX_CHARS 96
#define CONTENT_LINES 42

typedef enum {
    SECTION_OVERVIEW = 0,
    SECTION_LINUX,
    SECTION_SWM,
    SECTION_SRAPI,
    SECTION_COUNT,
} about_section_t;

typedef struct {
    about_section_t section;
    ranal_widget_t *section_btns[SECTION_COUNT];
    ranal_widget_t *title;
    ranal_widget_t *lines[CONTENT_LINES];
    int refresh_tick;
} about_state_t;

static const char *section_names[SECTION_COUNT] = {
    "Overview",
    "Linux",
    "SWM",
    "SRAPI",
};

static about_state_t g_app;

static void set_line(about_state_t *app, int idx, const char *fmt, ...) {
    if (idx < 0 || idx >= CONTENT_LINES || app->lines[idx] == NULL) return;
    char buf[LINE_MAX_CHARS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ranal_set_text(app->lines[idx], buf);
}

static int add_line(about_state_t *app, int idx, const char *fmt, ...) {
    if (idx < 0 || idx >= CONTENT_LINES) return idx;
    char buf[LINE_MAX_CHARS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_line(app, idx, "%s", buf);
    return idx + 1;
}

static void clear_lines(about_state_t *app) {
    for (int i = 0; i < CONTENT_LINES; i++) {
        set_line(app, i, "");
    }
}

static void clean_text(char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c == 127) *s = ' ';
    }
}

static char *trim(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static int os_release_value(const char *key, char *out, size_t out_sz) {
    FILE *f = fopen("/etc/os-release", "r");
    if (f == NULL) return 0;
    char line[256];
    size_t key_len = strlen(key);
    int ok = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') continue;
        char *v = trim(line + key_len + 1);
        if (v[0] == '"') {
            v++;
            char *q = strrchr(v, '"');
            if (q != NULL) *q = '\0';
        }
        snprintf(out, out_sz, "%s", v);
        clean_text(out);
        ok = 1;
        break;
    }
    fclose(f);
    return ok;
}

static void mem_info(unsigned long long *total_kb, unsigned long long *avail_kb) {
    *total_kb = 0;
    *avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) return;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        sscanf(line, "MemTotal: %llu kB", total_kb);
        sscanf(line, "MemAvailable: %llu kB", avail_kb);
        if (*total_kb > 0 && *avail_kb > 0) break;
    }
    fclose(f);
}

static void cpu_info(char *model, size_t model_sz, int *cores) {
    model[0] = '\0';
    *cores = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) return;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "processor", 9) == 0) (*cores)++;
        if (model[0] == '\0' &&
            (strncmp(line, "model name", 10) == 0 || strncmp(line, "Hardware", 8) == 0)) {
            char *p = strchr(line, ':');
            if (p != NULL) {
                p = trim(p + 1);
                snprintf(model, model_sz, "%s", p);
                clean_text(model);
            }
        }
    }
    fclose(f);
}

static void format_uptime(char *out, size_t out_sz) {
    struct sysinfo si;
    if (sysinfo(&si) != 0) {
        snprintf(out, out_sz, "unknown");
        return;
    }
    long days = si.uptime / 86400;
    long hours = (si.uptime / 3600) % 24;
    long mins = (si.uptime / 60) % 60;
    snprintf(out, out_sz, "%ldd %ldh %ldm", days, hours, mins);
}

static void swm_pid_list(char *out, size_t out_sz) {
    out[0] = '\0';
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        snprintf(out, out_sz, "unavailable");
        return;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        char path[96];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) continue;
        char cmd[512];
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if (n == 0) continue;
        cmd[n] = '\0';
        int has_swm_arg = 0;
        for (size_t i = 0; i < n;) {
            char *arg = &cmd[i];
            size_t len = strlen(arg);
            if (strcmp(arg, "swm") == 0) has_swm_arg = 1;
            i += len + 1;
        }
        if (!has_swm_arg) continue;
        char item[32];
        snprintf(item, sizeof(item), "%s%s", count > 0 ? "," : "", ent->d_name);
        if (strlen(out) + strlen(item) + 1 < out_sz) strcat(out, item);
        count++;
    }
    closedir(dir);
    if (count == 0) snprintf(out, out_sz, "not found");
}

static void probe_line(about_state_t *app, int *idx, srapi_backend_t backend) {
    srapi_device_info_t info;
    memset(&info, 0, sizeof(info));
    srapi_result_t r = srapi_probe_device(backend, &info);
    const char *name = srapi_backend_name(backend);
    if (r == SRAPI_OK && info.available) {
        *idx = add_line(app, *idx, "%s: available path=%s", name, info.path[0] ? info.path : "-");
    } else if (r == SRAPI_OK) {
        *idx = add_line(app, *idx, "%s: unavailable %s", name, info.message[0] ? info.message : "");
    } else {
        *idx = add_line(app, *idx, "%s: probe failed: %s", name, srapi_last_error());
    }
}

static void update_buttons(about_state_t *app) {
    for (int i = 0; i < SECTION_COUNT; i++) {
        ranal_set_foreground(app->section_btns[i], (about_section_t)i == app->section ? RANAL_ACCENT_HOT : RANAL_TEXT);
    }
}

static void show_overview(about_state_t *app) {
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    char os[128];
    if (!os_release_value("PRETTY_NAME", os, sizeof(os))) snprintf(os, sizeof(os), "%s", uts.sysname);

    char uptime[64];
    format_uptime(uptime, sizeof(uptime));

    int idx = 0;
    idx = add_line(app, idx, "OneTool %s", onetool_version);
    idx = add_line(app, idx, "System information viewer");
    idx = add_line(app, idx, "");
    idx = add_line(app, idx, "OS: %s", os);
    idx = add_line(app, idx, "Kernel: %s %s", uts.sysname, uts.release);
    idx = add_line(app, idx, "Machine: %s", uts.machine);
    idx = add_line(app, idx, "Uptime: %s", uptime);
    idx = add_line(app, idx, "");
    idx = add_line(app, idx, "SPROT: %u.%u", SPROT_VERSION_MAJOR, SPROT_VERSION_MINOR);
    idx = add_line(app, idx, "Window mode: %s", ranal_is_swm_mode() ? "SWM client" : "standalone");
    idx = add_line(app, idx, "Window size: %dx%d", ranal_window_width(), ranal_window_height());
    idx = add_line(app, idx, "SRAPI debug: %s", srapi ? "enabled" : "disabled");
}

static void show_linux(about_state_t *app) {
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));
    uname(&uts);

    char os[128];
    char host[128];
    char cpu[160];
    int cores = 0;
    unsigned long long mem_total = 0, mem_avail = 0;
    char uptime[64];
    if (!os_release_value("PRETTY_NAME", os, sizeof(os))) snprintf(os, sizeof(os), "unknown");
    if (gethostname(host, sizeof(host)) != 0) snprintf(host, sizeof(host), "unknown");
    host[sizeof(host) - 1] = '\0';
    cpu_info(cpu, sizeof(cpu), &cores);
    mem_info(&mem_total, &mem_avail);
    format_uptime(uptime, sizeof(uptime));

    int idx = 0;
    idx = add_line(app, idx, "Distribution: %s", os);
    idx = add_line(app, idx, "Hostname: %s", host);
    idx = add_line(app, idx, "Kernel: %s", uts.release);
    idx = add_line(app, idx, "Architecture: %s", uts.machine);
    idx = add_line(app, idx, "Uptime: %s", uptime);
    idx = add_line(app, idx, "");
    idx = add_line(app, idx, "CPU: %s", cpu[0] ? cpu : "unknown");
    idx = add_line(app, idx, "CPU cores: %d", cores > 0 ? cores : 1);
    if (mem_total > 0) {
        unsigned long long used = mem_total > mem_avail ? mem_total - mem_avail : 0;
        idx = add_line(app, idx, "Memory: %llu / %llu MB used",
                       used / 1024ull, mem_total / 1024ull);
        idx = add_line(app, idx, "Memory available: %llu MB", mem_avail / 1024ull);
    } else {
        idx = add_line(app, idx, "Memory: unknown");
    }
}

static void show_swm(about_state_t *app) {
    char pids[256];
    swm_pid_list(pids, sizeof(pids));
    int idx = 0;
    idx = add_line(app, idx, "SWM compositor");
    idx = add_line(app, idx, "Socket: %s", SPROT_DEFAULT_SOCKET);
    idx = add_line(app, idx, "SWM process pid(s): %s", pids);
    idx = add_line(app, idx, "Running as SWM client: %s", ranal_is_swm_mode() ? "yes" : "no");
    idx = add_line(app, idx, "This window: %dx%d", ranal_window_width(), ranal_window_height());
    idx = add_line(app, idx, "Protocol: sprot %u.%u", SPROT_VERSION_MAJOR, SPROT_VERSION_MINOR);
    idx = add_line(app, idx, "");
    idx = add_line(app, idx, "Window list: not exposed by sprot yet");
}

static void show_srapi(about_state_t *app) {
    srapi_shade_config_t shade = srapi_get_shade_config();
    int idx = 0;
    idx = add_line(app, idx, "SRAPI");
    idx = add_line(app, idx, "Debug: %s", srapi ? "enabled" : "disabled");
    idx = add_line(app, idx, "Last error: %s", srapi_last_error());
    idx = add_line(app, idx, "Shader SIMD: %s", shade.simd_enabled ? "on" : "off");
    idx = add_line(app, idx, "Shader threads: %s", shade.threads_enabled ? "on" : "off");
    idx = add_line(app, idx, "");
    probe_line(app, &idx, SRAPI_BACKEND_CPU);
    probe_line(app, &idx, SRAPI_BACKEND_GPU);
    probe_line(app, &idx, SRAPI_BACKEND_FBDEV);
}

static void refresh_content(about_state_t *app) {
    clear_lines(app);
    ranal_set_text(app->title, section_names[app->section]);
    update_buttons(app);

    switch (app->section) {
        case SECTION_OVERVIEW: show_overview(app); break;
        case SECTION_LINUX:    show_linux(app); break;
        case SECTION_SWM:      show_swm(app); break;
        case SECTION_SRAPI:    show_srapi(app); break;
        default: break;
    }
}

static void on_section(ranal_widget_t *w, void *user) {
    about_state_t *app = user;
    for (int i = 0; i < SECTION_COUNT; i++) {
        if (app->section_btns[i] == w) {
            app->section = (about_section_t)i;
            refresh_content(app);
            return;
        }
    }
}

static void on_close(ranal_widget_t *w, void *user) {
    (void)w;
    (void)user;
    ranal_request_close();
}

static void usage(const char *argv0) {
    printf("usage: %s [--swm [WxH]] [--title TITLE] [--debug]\n", argv0);
}

int main(int argc, char *argv[]) {
    int swm_mode = 0;
    int32_t swm_w = ABOUT_W;
    int32_t swm_h = ABOUT_H;
    const char *title = "About system";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--swm") == 0) {
            swm_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int w = 0, h = 0;
                if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    swm_w = w;
                    swm_h = h;
                    i++;
                }
            }
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            srapi = 1;
        }
    }

    if (swm_mode) {
        if (ranal_init_swm(title, swm_w, swm_h) != RANAL_OK) {
            fprintf(stderr, "about: %s\n", ranal_last_error());
            return 1;
        }
    } else {
        ranal_window_desc_t desc = { .width = ABOUT_W, .height = ABOUT_H, .title = title };
        if (ranal_init(&desc) != RANAL_OK) {
            fprintf(stderr, "about: %s\n", ranal_last_error());
            return 1;
        }
    }

    memset(&g_app, 0, sizeof(g_app));

    ranal_widget_t *root = ranal_root();
    ranal_set_layout(root, RANAL_LAYOUT_ABSOLUTE);
    ranal_set_background(root, RANAL_COLOR(18, 20, 26));

    ranal_widget_t *sidebar = ranal_panel(root);
    ranal_set_pos(sidebar, 0, 0);
    ranal_set_size(sidebar, SIDEBAR_W, ranal_window_height());
    ranal_set_background(sidebar, RANAL_COLOR(28, 32, 42));

    ranal_widget_t *brand = ranal_label(root, "OneTool");
    ranal_set_pos(brand, 16, 16);
    ranal_set_foreground(brand, RANAL_ACCENT_HOT);

    for (int i = 0; i < SECTION_COUNT; i++) {
        ranal_widget_t *btn = ranal_button(root, section_names[i]);
        ranal_set_pos(btn, 12, 52 + i * 34);
        ranal_set_size(btn, SIDEBAR_W - 24, 26);
        ranal_on_click(btn, on_section, &g_app);
        g_app.section_btns[i] = btn;
    }

    ranal_widget_t *close_btn = ranal_button(root, "Close");
    ranal_set_pos(close_btn, 12, ranal_window_height() - 40);
    ranal_set_size(close_btn, SIDEBAR_W - 24, 26);
    ranal_on_click(close_btn, on_close, NULL);

    ranal_widget_t *content = ranal_panel(root);
    ranal_set_pos(content, SIDEBAR_W, 0);
    ranal_set_size(content, ranal_window_width() - SIDEBAR_W, ranal_window_height());
    ranal_set_background(content, RANAL_COLOR(22, 24, 30));

    g_app.title = ranal_label(root, "Overview");
    ranal_set_pos(g_app.title, SIDEBAR_W + 22, 18);
    ranal_set_foreground(g_app.title, RANAL_ACCENT_HOT);

    for (int i = 0; i < CONTENT_LINES; i++) {
        ranal_widget_t *line = ranal_label(root, "");
        ranal_set_pos(line, SIDEBAR_W + 22, 52 + i * RANAL_FONT_ADVANCE_Y);
        ranal_set_foreground(line, RANAL_TEXT);
        g_app.lines[i] = line;
    }

    refresh_content(&g_app);

    while (!ranal_should_close()) {
        if ((g_app.refresh_tick++ % 30) == 0) {
            refresh_content(&g_app);
        }
        if (ranal_render() != 0) break;
        if (ranal_present() != 0) break;
    }

    ranal_shutdown();
    return 0;
}
