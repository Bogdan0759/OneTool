#include "config/tool_registry.h"
#include "libs/TUI/tui.h"
#include "tools/system/taskmng/taskmng.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TUI_MAX_FIELDS 16
#define TUI_MAX_OPTIONS 16
#define TUI_MAX_VALUE 256
#define TUI_MAX_LABEL 96
#define TUI_MAX_ARGS 64
#define TUI_MAX_TOKEN 256
#define TUI_THEME_COUNT 8

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum onetool_form_field_type {
    ONETOOL_FIELD_TEXT = 0,
    ONETOOL_FIELD_TOGGLE = 1,
    ONETOOL_FIELD_CHOICE = 2,
};

typedef struct {
    char value[64];
    char label[64];
} onetool_choice_option_t;

typedef struct {
    char name[48];
    char label[TUI_MAX_LABEL];
    char flag[24];
    char value[TUI_MAX_VALUE];
    char default_value[TUI_MAX_VALUE];
    char placeholder[96];
    char help[160];
    int type;
    int option_count;
    int choice_index;
    onetool_choice_option_t options[TUI_MAX_OPTIONS];
} onetool_form_field_t;

typedef struct {
    char title[128];
    char summary[256];
    int has_config;
    int field_count;
    onetool_form_field_t fields[TUI_MAX_FIELDS];
} onetool_form_t;

typedef struct {
    char name[48];
    tui_palette_t palette;
} onetool_theme_t;

typedef struct {
    int screen;
    int selected_tool;
    int tool_scroll;
    int selected_field;
    int current_theme;
    char status[160];
    char launch_dir[PATH_MAX];
    onetool_form_t form;
    onetool_form_t settings_form;
    const struct onetool_tool *active_tool;
    int settings_selected_field;
    int settings_return_screen;
    taskmng_runtime_t taskmng_runtime;
    taskmng_snapshot_t taskmng_snapshot;
    taskmng_view_t taskmng_view;
} onetool_tui_state_t;

enum onetool_screen {
    ONETOOL_SCREEN_TOOLS = 0,
    ONETOOL_SCREEN_FORM = 1,
    ONETOOL_SCREEN_SETTINGS = 2,
};

static onetool_theme_t g_themes[TUI_THEME_COUNT];
static int g_theme_count = 0;

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static int string_is_true(const char *text) {
    if (text == NULL) {
        return 0;
    }
    return strcmp(text, "1") == 0 ||
           strcmp(text, "true") == 0 ||
           strcmp(text, "yes") == 0 ||
           strcmp(text, "on") == 0;
}

static const char *color_name(int color) {
    switch (color) {
        case TUI_COLOR_BLACK:
            return "black";
        case TUI_COLOR_RED:
            return "red";
        case TUI_COLOR_GREEN:
            return "green";
        case TUI_COLOR_YELLOW:
            return "yellow";
        case TUI_COLOR_BLUE:
            return "blue";
        case TUI_COLOR_MAGENTA:
            return "magenta";
        case TUI_COLOR_CYAN:
            return "cyan";
        case TUI_COLOR_WHITE:
            return "white";
        case TUI_COLOR_DEFAULT:
        default:
            return "default";
    }
}

static int parse_color_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return TUI_COLOR_DEFAULT;
    }
    if (strcmp(name, "default") == 0) return TUI_COLOR_DEFAULT;
    if (strcmp(name, "black") == 0) return TUI_COLOR_BLACK;
    if (strcmp(name, "red") == 0) return TUI_COLOR_RED;
    if (strcmp(name, "green") == 0) return TUI_COLOR_GREEN;
    if (strcmp(name, "yellow") == 0) return TUI_COLOR_YELLOW;
    if (strcmp(name, "blue") == 0) return TUI_COLOR_BLUE;
    if (strcmp(name, "magenta") == 0) return TUI_COLOR_MAGENTA;
    if (strcmp(name, "cyan") == 0) return TUI_COLOR_CYAN;
    if (strcmp(name, "white") == 0) return TUI_COLOR_WHITE;
    return TUI_COLOR_DEFAULT;
}

static void init_embedded_themes(void) {
    g_theme_count = 5;

    copy_string(g_themes[0].name, sizeof(g_themes[0].name), "ocean");
    g_themes[0].palette = (tui_palette_t){ TUI_COLOR_BLACK, TUI_COLOR_BLUE, TUI_COLOR_CYAN, TUI_COLOR_WHITE, TUI_COLOR_CYAN, TUI_COLOR_GREEN, TUI_COLOR_RED };

    copy_string(g_themes[1].name, sizeof(g_themes[1].name), "forest");
    g_themes[1].palette = (tui_palette_t){ TUI_COLOR_BLACK, TUI_COLOR_GREEN, TUI_COLOR_YELLOW, TUI_COLOR_WHITE, TUI_COLOR_CYAN, TUI_COLOR_GREEN, TUI_COLOR_RED };

    copy_string(g_themes[2].name, sizeof(g_themes[2].name), "amber");
    g_themes[2].palette = (tui_palette_t){ TUI_COLOR_BLACK, TUI_COLOR_RED, TUI_COLOR_YELLOW, TUI_COLOR_WHITE, TUI_COLOR_MAGENTA, TUI_COLOR_GREEN, TUI_COLOR_RED };

    copy_string(g_themes[3].name, sizeof(g_themes[3].name), "mono");
    g_themes[3].palette = (tui_palette_t){ TUI_COLOR_BLACK, TUI_COLOR_WHITE, TUI_COLOR_WHITE, TUI_COLOR_WHITE, TUI_COLOR_CYAN, TUI_COLOR_GREEN, TUI_COLOR_RED };

    copy_string(g_themes[4].name, sizeof(g_themes[4].name), "custom");
    g_themes[4].palette = g_themes[0].palette;
}

static void set_status(onetool_tui_state_t *state, const char *text) {
    copy_string(state->status, sizeof(state->status), text);
}

static int tool_is_taskmng(const struct onetool_tool *tool) {
    return tool != NULL && strcmp(tool->name, "taskmng") == 0;
}

static void init_default_form(onetool_form_t *form, const struct onetool_tool *tool) {
    memset(form, 0, sizeof(*form));
    copy_string(form->title, sizeof(form->title), tool->name);
    copy_string(form->summary, sizeof(form->summary), tool->description);
    form->has_config = 0;
}

static int add_form_field(onetool_form_t *form) {
    if (form->field_count >= TUI_MAX_FIELDS) {
        return -1;
    }
    memset(&form->fields[form->field_count], 0, sizeof(form->fields[form->field_count]));
    return form->field_count++;
}

static void sync_choice_field(onetool_form_field_t *field) {
    if (field->type != ONETOOL_FIELD_CHOICE || field->option_count == 0) {
        return;
    }

    if (field->choice_index < 0 || field->choice_index >= field->option_count) {
        field->choice_index = 0;
    }
    copy_string(field->value, sizeof(field->value), field->options[field->choice_index].value);
}

static void add_extra_args_field(onetool_form_t *form) {
    int idx = add_form_field(form);
    if (idx < 0) {
        return;
    }

    copy_string(form->fields[idx].name, sizeof(form->fields[idx].name), "extra_args");
    copy_string(form->fields[idx].label, sizeof(form->fields[idx].label), "Extra args");
    copy_string(form->fields[idx].flag, sizeof(form->fields[idx].flag), "@extra");
    copy_string(form->fields[idx].placeholder, sizeof(form->fields[idx].placeholder), "--help");
    copy_string(form->fields[idx].help, sizeof(form->fields[idx].help), "Extra CLI args appended after the generated form values.");
    form->fields[idx].type = ONETOOL_FIELD_TEXT;
}

static onetool_form_field_t *add_text_field(
    onetool_form_t *form,
    const char *name,
    const char *label,
    const char *flag,
    const char *default_value,
    const char *placeholder,
    const char *help
) {
    int idx = add_form_field(form);
    if (idx < 0) {
        return NULL;
    }

    copy_string(form->fields[idx].name, sizeof(form->fields[idx].name), name);
    copy_string(form->fields[idx].label, sizeof(form->fields[idx].label), label);
    copy_string(form->fields[idx].flag, sizeof(form->fields[idx].flag), flag);
    copy_string(form->fields[idx].default_value, sizeof(form->fields[idx].default_value), default_value);
    copy_string(form->fields[idx].value, sizeof(form->fields[idx].value), default_value);
    copy_string(form->fields[idx].placeholder, sizeof(form->fields[idx].placeholder), placeholder);
    copy_string(form->fields[idx].help, sizeof(form->fields[idx].help), help);
    form->fields[idx].type = ONETOOL_FIELD_TEXT;
    return &form->fields[idx];
}

static onetool_form_field_t *add_toggle_field(
    onetool_form_t *form,
    const char *name,
    const char *label,
    const char *flag,
    int enabled_by_default,
    const char *help
) {
    int idx = add_form_field(form);
    if (idx < 0) {
        return NULL;
    }

    copy_string(form->fields[idx].name, sizeof(form->fields[idx].name), name);
    copy_string(form->fields[idx].label, sizeof(form->fields[idx].label), label);
    copy_string(form->fields[idx].flag, sizeof(form->fields[idx].flag), flag);
    copy_string(form->fields[idx].default_value, sizeof(form->fields[idx].default_value), enabled_by_default ? "1" : "0");
    copy_string(form->fields[idx].value, sizeof(form->fields[idx].value), enabled_by_default ? "1" : "0");
    copy_string(form->fields[idx].help, sizeof(form->fields[idx].help), help);
    form->fields[idx].type = ONETOOL_FIELD_TOGGLE;
    return &form->fields[idx];
}

static onetool_form_field_t *add_choice_field(
    onetool_form_t *form,
    const char *name,
    const char *label,
    const char *flag,
    const char *default_value,
    const char *help
) {
    int idx = add_form_field(form);
    if (idx < 0) {
        return NULL;
    }

    copy_string(form->fields[idx].name, sizeof(form->fields[idx].name), name);
    copy_string(form->fields[idx].label, sizeof(form->fields[idx].label), label);
    copy_string(form->fields[idx].flag, sizeof(form->fields[idx].flag), flag);
    copy_string(form->fields[idx].default_value, sizeof(form->fields[idx].default_value), default_value);
    copy_string(form->fields[idx].value, sizeof(form->fields[idx].value), default_value);
    copy_string(form->fields[idx].help, sizeof(form->fields[idx].help), help);
    form->fields[idx].type = ONETOOL_FIELD_CHOICE;
    return &form->fields[idx];
}

static void add_choice_option(onetool_form_field_t *field, const char *value, const char *label) {
    onetool_choice_option_t *option;

    if (field == NULL || field->option_count >= TUI_MAX_OPTIONS) {
        return;
    }

    option = &field->options[field->option_count++];
    copy_string(option->value, sizeof(option->value), value);
    copy_string(option->label, sizeof(option->label), label);
    if (strcmp(value, field->default_value) == 0) {
        field->choice_index = field->option_count - 1;
    }
}

static void add_all_color_options(onetool_form_field_t *field) {
    add_choice_option(field, "default", "default");
    add_choice_option(field, "black", "black");
    add_choice_option(field, "red", "red");
    add_choice_option(field, "green", "green");
    add_choice_option(field, "yellow", "yellow");
    add_choice_option(field, "blue", "blue");
    add_choice_option(field, "magenta", "magenta");
    add_choice_option(field, "cyan", "cyan");
    add_choice_option(field, "white", "white");
    sync_choice_field(field);
}

static onetool_form_field_t *find_form_field(onetool_form_t *form, const char *name) {
    if (form == NULL || name == NULL) {
        return NULL;
    }

    for (int i = 0; i < form->field_count; i++) {
        if (strcmp(form->fields[i].name, name) == 0) {
            return &form->fields[i];
        }
    }
    return NULL;
}

static void load_embedded_tool_form(onetool_form_t *form, const struct onetool_tool *tool) {
    onetool_form_field_t *field = NULL;

    init_default_form(form, tool);
    form->has_config = 1;

    if (strcmp(tool->name, "lastmod") == 0) {
        copy_string(form->title, sizeof(form->title), "Last Modification Time");
        copy_string(form->summary, sizeof(form->summary), "Inspect a file and print its last modification time.");
        add_text_field(form, "path", "Path", "", "/etc/hosts", "/path/to/file", "File to inspect.");
    } else if (strcmp(tool->name, "exec") == 0) {
        copy_string(form->title, sizeof(form->title), "Execute File");
        copy_string(form->summary, sizeof(form->summary), "Run a file directly or launch it through a chosen interpreter.");
        add_text_field(form, "path", "File path", "", "./script.sh", "./script.sh", "Program or script to execute.");
        add_text_field(form, "interpreter", "Interpreter", "-i", "", "<empty = direct binary>", "Leave this empty to run a binary directly without any interpreter.");
    } else if (strcmp(tool->name, "fsinfo") == 0) {
        copy_string(form->title, sizeof(form->title), "Filesystem Info");
        copy_string(form->summary, sizeof(form->summary), "Inspect a path and print its filesystem and inode information.");
        add_text_field(form, "path", "Path", "", "/", "/path/to/dir", "Path to inspect.");
    } else if (strcmp(tool->name, "gapi_supported") == 0) {
        copy_string(form->title, sizeof(form->title), "gapi check");
        copy_string(form->summary, sizeof(form->summary), "check all available graphic API");
    } else if (strcmp(tool->name, "down") == 0) {
        copy_string(form->title, sizeof(form->title), "HTTP Downloader");
        copy_string(form->summary, sizeof(form->summary), "Download an HTTP resource with common OneTool options. Advanced flags can still be added through Extra args.");
        add_text_field(form, "url", "URL", "", "https://example.com", "https://example.com", "Target URL.");
        add_text_field(form, "output", "Output file", "-o", "", "download.bin", "Optional file path for the response body.");
        field = add_choice_field(form, "method", "Method", "-X", "GET", "HTTP method.");
        add_choice_option(field, "GET", "GET");
        add_choice_option(field, "POST", "POST");
        add_choice_option(field, "HEAD", "HEAD");
        sync_choice_field(field);
        add_text_field(form, "body", "Body", "-d", "", "name=value", "Optional request body.");
        add_text_field(form, "timeout", "Timeout", "--timeout", "10", "10", "Connect timeout in seconds.");
        add_toggle_field(form, "ignore_robots", "Ignore robots", "--ignore-robots", 0, "Ignore robots.txt rules.");
        add_toggle_field(form, "verbose", "Verbose", "-v", 0, "Print diagnostics to stderr.");
        add_toggle_field(form, "quiet", "Quiet", "-q", 0, "Hide normal status lines.");
    } else if (strcmp(tool->name, "ping") == 0) {
        copy_string(form->title, sizeof(form->title), "Ping");
        copy_string(form->summary, sizeof(form->summary), "Send ICMP echo requests and display round-trip stats.");
        add_text_field(form, "host", "Host", "", "1.1.1.1", "example.com", "Host or IPv4 address.");
        add_text_field(form, "count", "Count", "-c", "4", "4", "Number of echo requests.");
        add_text_field(form, "interval", "Interval", "-i", "1", "1", "Delay between packets in seconds.");
        add_text_field(form, "timeout", "Timeout ms", "-W", "1000", "1000", "Per-packet timeout in milliseconds.");
        add_text_field(form, "size", "Payload bytes", "-s", "56", "56", "Payload size.");
        add_toggle_field(form, "quiet", "Quiet summary", "-q", 0, "Print only the summary.");
    } else if (strcmp(tool->name, "lmake") == 0) {
        copy_string(form->title, sizeof(form->title), "LMake");
        copy_string(form->summary, sizeof(form->summary), "Run the bundled lmake build tool from TUI. Set the main target here, then use Extra args for any additional lmake flags.");
        add_text_field(form, "target", "Target", "", "build", "build", "Main lmake target or task name.");
    } else if (strcmp(tool->name, "lpack") == 0) {
        copy_string(form->title, sizeof(form->title), "Lua Packager");
        copy_string(form->summary, sizeof(form->summary), "Bundle a Lua script and interpreter into a runnable ELF file.");
        add_text_field(form, "input", "Lua script", "", "main.lua", "main.lua", "Input Lua script.");
        add_text_field(form, "interpreter", "Interpreter", "-i", "", "/usr/bin/lua", "Path to the Lua interpreter.");
        add_text_field(form, "output", "Output file", "-o", "a.out", "bundle.out", "Generated ELF file.");
    } else if (strcmp(tool->name, "reboot") == 0) {
        copy_string(form->title, sizeof(form->title), "Reboot");
        copy_string(form->summary, sizeof(form->summary), "Reboot the system now or after a delay.");
        add_text_field(form, "delay", "Delay seconds", "-t", "", "5", "Optional delay before reboot.");
    } else if (strcmp(tool->name, "shutdown") == 0) {
        copy_string(form->title, sizeof(form->title), "Shutdown");
        copy_string(form->summary, sizeof(form->summary), "Power off the system now or after a delay.");
        add_text_field(form, "delay", "Delay seconds", "-t", "", "5", "Optional delay before shutdown.");
    } else {
        form->has_config = 0;
    }

    add_extra_args_field(form);
}

static void cycle_theme(onetool_tui_state_t *state, int direction) {
    if (g_theme_count <= 0) {
        return;
    }

    state->current_theme += direction;
    if (state->current_theme < 0) {
        state->current_theme = g_theme_count - 1;
    }
    if (state->current_theme >= g_theme_count) {
        state->current_theme = 0;
    }
    tui_set_palette(&g_themes[state->current_theme].palette);
    snprintf(state->status, sizeof(state->status), "Theme: %s", g_themes[state->current_theme].name);
}

static void set_choice_field_value(onetool_form_field_t *field, const char *value) {
    if (field == NULL || field->type != ONETOOL_FIELD_CHOICE) {
        return;
    }

    for (int i = 0; i < field->option_count; i++) {
        if (strcmp(field->options[i].value, value) == 0) {
            field->choice_index = i;
            sync_choice_field(field);
            return;
        }
    }
}

static void apply_palette_to_settings_form(onetool_form_t *form, const tui_palette_t *palette) {
    set_choice_field_value(find_form_field(form, "background"), color_name(palette->background));
    set_choice_field_value(find_form_field(form, "panel"), color_name(palette->panel));
    set_choice_field_value(find_form_field(form, "accent"), color_name(palette->accent));
    set_choice_field_value(find_form_field(form, "text"), color_name(palette->text));
    set_choice_field_value(find_form_field(form, "muted"), color_name(palette->muted));
    set_choice_field_value(find_form_field(form, "success"), color_name(palette->success));
    set_choice_field_value(find_form_field(form, "error"), color_name(palette->error));
}

static void open_settings(onetool_tui_state_t *state, int return_screen) {
    onetool_form_field_t *field;

    memset(&state->settings_form, 0, sizeof(state->settings_form));
    copy_string(state->settings_form.title, sizeof(state->settings_form.title), "Settings");
    copy_string(
        state->settings_form.summary,
        sizeof(state->settings_form.summary),
        "Change the working directory used for tool launches and tune the active TUI palette live."
    );
    state->settings_form.has_config = 1;

    add_text_field(
        &state->settings_form,
        "launch_dir",
        "Launch path",
        "",
        state->launch_dir[0] != '\0' ? state->launch_dir : ".",
        "/path/to/project",
        "Directory where tools will be started."
    );

    field = add_choice_field(&state->settings_form, "theme_preset", "Theme preset", "", g_themes[state->current_theme].name, "Preset palette. Editing colors moves you to custom.");
    add_choice_option(field, "ocean", "ocean");
    add_choice_option(field, "forest", "forest");
    add_choice_option(field, "amber", "amber");
    add_choice_option(field, "mono", "mono");
    add_choice_option(field, "custom", "custom");
    sync_choice_field(field);

    field = add_choice_field(&state->settings_form, "background", "Background", "", color_name(g_themes[state->current_theme].palette.background), "Terminal background color.");
    add_all_color_options(field);
    field = add_choice_field(&state->settings_form, "panel", "Panel", "", color_name(g_themes[state->current_theme].palette.panel), "Panel and status bar background.");
    add_all_color_options(field);
    field = add_choice_field(&state->settings_form, "accent", "Accent", "", color_name(g_themes[state->current_theme].palette.accent), "Highlights and selected tool name.");
    add_all_color_options(field);
    field = add_choice_field(&state->settings_form, "text", "Text", "", color_name(g_themes[state->current_theme].palette.text), "Primary text color.");
    add_all_color_options(field);
    field = add_choice_field(&state->settings_form, "muted", "Muted", "", color_name(g_themes[state->current_theme].palette.muted), "Secondary and hint text color.");
    add_all_color_options(field);
    field = add_choice_field(&state->settings_form, "success", "Success", "", color_name(g_themes[state->current_theme].palette.success), "Success color.");
    add_all_color_options(field);
    field = add_choice_field(&state->settings_form, "error", "Error", "", color_name(g_themes[state->current_theme].palette.error), "Error color.");
    add_all_color_options(field);

    state->settings_selected_field = 0;
    state->settings_return_screen = return_screen;
    state->screen = ONETOOL_SCREEN_SETTINGS;
    set_status(state, "Settings: edit launch path and palette, then press N or Esc to return.");
}

static void close_settings(onetool_tui_state_t *state) {
    onetool_form_field_t *launch_dir_field = find_form_field(&state->settings_form, "launch_dir");

    if (launch_dir_field != NULL) {
        copy_string(state->launch_dir, sizeof(state->launch_dir), launch_dir_field->value[0] != '\0' ? launch_dir_field->value : ".");
    }
    state->screen = state->settings_return_screen;
    set_status(state, "Settings applied.");
}

static void sync_custom_palette_from_settings(onetool_tui_state_t *state) {
    tui_palette_t *palette = &g_themes[4].palette;

    palette->background = parse_color_name(find_form_field(&state->settings_form, "background")->value);
    palette->panel = parse_color_name(find_form_field(&state->settings_form, "panel")->value);
    palette->accent = parse_color_name(find_form_field(&state->settings_form, "accent")->value);
    palette->text = parse_color_name(find_form_field(&state->settings_form, "text")->value);
    palette->muted = parse_color_name(find_form_field(&state->settings_form, "muted")->value);
    palette->success = parse_color_name(find_form_field(&state->settings_form, "success")->value);
    palette->error = parse_color_name(find_form_field(&state->settings_form, "error")->value);
    state->current_theme = 4;
    tui_set_palette(palette);
    set_choice_field_value(find_form_field(&state->settings_form, "theme_preset"), "custom");
}

static void apply_theme_preset_from_settings(onetool_tui_state_t *state) {
    onetool_form_field_t *field = find_form_field(&state->settings_form, "theme_preset");

    if (field == NULL || field->type != ONETOOL_FIELD_CHOICE) {
        return;
    }
    for (int i = 0; i < g_theme_count; i++) {
        if (strcmp(g_themes[i].name, field->value) == 0) {
            state->current_theme = i;
            tui_set_palette(&g_themes[i].palette);
            apply_palette_to_settings_form(&state->settings_form, &g_themes[i].palette);
            snprintf(state->status, sizeof(state->status), "Theme preset: %s", g_themes[i].name);
            return;
        }
    }
}

static void ensure_custom_theme(onetool_tui_state_t *state) {
    if (state->current_theme != 4) {
        g_themes[4].palette = g_themes[state->current_theme].palette;
        state->current_theme = 4;
        apply_palette_to_settings_form(&state->settings_form, &g_themes[4].palette);
        set_choice_field_value(find_form_field(&state->settings_form, "theme_preset"), "custom");
    }
}

static int tokenize_extra_args(const char *text, char tokens[][TUI_MAX_TOKEN], int max_tokens) {
    int count = 0;
    int in_single = 0;
    int in_double = 0;
    int escaping = 0;
    int out_len = 0;

    if (text == NULL) {
        return 0;
    }

    memset(tokens, 0, (size_t)max_tokens * TUI_MAX_TOKEN);

    for (const char *p = text; ; p++) {
        char ch = *p;

        if (escaping) {
            if (count >= max_tokens) {
                return -1;
            }
            if (out_len < TUI_MAX_TOKEN - 1) {
                tokens[count][out_len++] = ch;
            }
            escaping = 0;
            continue;
        }

        if (ch == '\\' && ch != '\0') {
            escaping = 1;
            continue;
        }
        if (ch == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (ch == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }

        if (ch == '\0' || (!in_single && !in_double && isspace((unsigned char)ch))) {
            if (out_len > 0) {
                if (count >= max_tokens) {
                    return -1;
                }
                tokens[count][out_len] = '\0';
                count++;
                out_len = 0;
            }
            if (ch == '\0') {
                break;
            }
            continue;
        }

        if (count >= max_tokens) {
            return -1;
        }
        if (out_len < TUI_MAX_TOKEN - 1) {
            tokens[count][out_len++] = ch;
        }
    }

    if (escaping || in_single || in_double) {
        return -1;
    }
    return count;
}

static int build_tool_argv(
    const struct onetool_tool *tool,
    const onetool_form_t *form,
    char argv_storage[][TUI_MAX_TOKEN],
    char *argv[],
    char error_text[],
    size_t error_size
) {
    char extra_tokens[TUI_MAX_ARGS][TUI_MAX_TOKEN];
    int argc = 1;

    copy_string(argv_storage[0], TUI_MAX_TOKEN, tool->name);
    argv[0] = argv_storage[0];

    for (int i = 0; i < form->field_count; i++) {
        const onetool_form_field_t *field = &form->fields[i];
        if (strcmp(field->flag, "@extra") == 0) {
            int extra_count = tokenize_extra_args(field->value, extra_tokens, TUI_MAX_ARGS);
            if (extra_count < 0) {
                copy_string(error_text, error_size, "Extra args contain an unfinished quote or too many tokens.");
                return -1;
            }
            for (int j = 0; j < extra_count && argc < TUI_MAX_ARGS - 1; j++) {
                copy_string(argv_storage[argc], TUI_MAX_TOKEN, extra_tokens[j]);
                argv[argc] = argv_storage[argc];
                argc++;
            }
            continue;
        }

        if (field->type == ONETOOL_FIELD_TOGGLE) {
            if (string_is_true(field->value) && field->flag[0] != '\0' && argc < TUI_MAX_ARGS - 1) {
                copy_string(argv_storage[argc], TUI_MAX_TOKEN, field->flag);
                argv[argc] = argv_storage[argc];
                argc++;
            }
            continue;
        }

        if (field->value[0] == '\0') {
            continue;
        }
        if (field->flag[0] != '\0' && argc < TUI_MAX_ARGS - 1) {
            copy_string(argv_storage[argc], TUI_MAX_TOKEN, field->flag);
            argv[argc] = argv_storage[argc];
            argc++;
        }
        if (argc < TUI_MAX_ARGS - 1) {
            copy_string(argv_storage[argc], TUI_MAX_TOKEN, field->value);
            argv[argc] = argv_storage[argc];
            argc++;
        }
    }

    argv[argc] = NULL;
    error_text[0] = '\0';
    return argc;
}

static void build_preview_text(const struct onetool_tool *tool, const onetool_form_t *form, char *dst, size_t dst_size) {
    char argv_storage[TUI_MAX_ARGS][TUI_MAX_TOKEN];
    char *argv[TUI_MAX_ARGS];
    char error_text[160];
    int argc = build_tool_argv(tool, form, argv_storage, argv, error_text, sizeof(error_text));
    size_t used = 0;

    if (argc < 0) {
        copy_string(dst, dst_size, error_text);
        return;
    }

    dst[0] = '\0';
    for (int i = 0; i < argc; i++) {
        int written = snprintf(dst + used, dst_size - used, "%s%s", i == 0 ? "" : " ", argv[i]);
        if (written < 0 || (size_t)written >= dst_size - used) {
            break;
        }
        used += (size_t)written;
    }
}

static void draw_wrapped_text(int x, int y, int width, int max_lines, int style, const char *text) {
    int line = 0;
    const char *cursor = text;

    if (text == NULL || width <= 0 || max_lines <= 0) {
        return;
    }

    while (*cursor != '\0' && line < max_lines) {
        char chunk[256];
        int chunk_len = 0;

        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        while (*cursor != '\0' && chunk_len < width) {
            if (*cursor == '\n') {
                cursor++;
                break;
            }
            chunk[chunk_len++] = *cursor++;
            if (chunk_len == width) {
                break;
            }
        }
        chunk[chunk_len] = '\0';
        tui_draw_text(x, y + line, style, chunk);
        line++;
    }
}

static void ensure_tool_visible(onetool_tui_state_t *state, int visible_rows) {
    if (state->selected_tool < state->tool_scroll) {
        state->tool_scroll = state->selected_tool;
    }
    if (state->selected_tool >= state->tool_scroll + visible_rows) {
        state->tool_scroll = state->selected_tool - visible_rows + 1;
    }
    if (state->tool_scroll < 0) {
        state->tool_scroll = 0;
    }
}

static void draw_tool_screen(onetool_tui_state_t *state, int width, int height) {
    int left_width = width < 80 ? width / 2 : 30;
    int right_x = left_width + 1;
    int right_width = width - right_x;
    int visible_rows = height - 6;
    const struct onetool_tool *selected_tool = onetool_get_tool_by_index(state->selected_tool);
    char preview[256];

    if (visible_rows < 3) {
        visible_rows = 3;
    }
    ensure_tool_visible(state, visible_rows - 2);

    tui_draw_box(0, 0, left_width, height - 1, TUI_STYLE_PANEL, "Tools");
    tui_draw_box(right_x, 0, right_width, height - 1, TUI_STYLE_PANEL, "Details");

    for (int row = 0; row < visible_rows - 2; row++) {
        int index = state->tool_scroll + row;
        const struct onetool_tool *tool = onetool_get_tool_by_index(index);
        char label[128];
        if (tool == NULL) {
            break;
        }
        snprintf(label, sizeof(label), "%s", tool->name);
        tui_draw_text(2, 1 + row, index == state->selected_tool ? TUI_STYLE_SELECTION : TUI_STYLE_NORMAL, label);
    }

    tui_draw_textf(2, height - 3, TUI_STYLE_MUTED, "%d tools", onetool_total_tool_count());
    tui_draw_textf(right_x + 2, 2, TUI_STYLE_ACCENT, "%s", selected_tool != NULL ? selected_tool->name : "none");
    if (selected_tool != NULL) {
        if (tool_is_taskmng(selected_tool)) {
            int taskmng_ok = state->taskmng_runtime.has_snapshot
                ? taskmng_refresh(&state->taskmng_runtime, &state->taskmng_snapshot)
                : taskmng_collect(&state->taskmng_runtime, &state->taskmng_snapshot);

            if (taskmng_ok) {
                taskmng_draw_panel(&state->taskmng_snapshot, &state->taskmng_view, right_x + 2, 4, right_width - 4, height - 9, 1);
            } else {
                tui_draw_text(right_x + 2, 4, TUI_STYLE_ERROR, "taskmng: failed to read /proc");
            }
            tui_draw_text(right_x + 2, height - 4, TUI_STYLE_MUTED, "W/S scroll | k kill | t term | f freeze | g rescan | h filter");
        } else {
            draw_wrapped_text(right_x + 2, 4, right_width - 4, 4, TUI_STYLE_NORMAL, selected_tool->description);
            tui_draw_textf(right_x + 2, 10, TUI_STYLE_MUTED, "Theme: %s", g_themes[state->current_theme].name);
            tui_draw_text(right_x + 2, 11, TUI_STYLE_MUTED, "Run path:");
            tui_draw_text(right_x + 2, 12, TUI_STYLE_NORMAL, state->launch_dir);
            tui_draw_text(right_x + 2, 14, TUI_STYLE_NORMAL, "Press Enter to open tool settings.");
            tui_draw_text(right_x + 2, 15, TUI_STYLE_NORMAL, "Press N for global settings.");
            tui_draw_text(right_x + 2, 16, TUI_STYLE_NORMAL, "Press T to switch themes quickly.");
            tui_draw_text(right_x + 2, 17, TUI_STYLE_NORMAL, "Use arrows/PageUp/PageDown to move.");
            snprintf(preview, sizeof(preview), "Command: onetool %s", selected_tool->name);
            tui_draw_text(right_x + 2, height - 4, TUI_STYLE_MUTED, preview);
        }
    }
}

static void format_field_value(const onetool_form_field_t *field, char *dst, size_t dst_size) {
    if (field->type == ONETOOL_FIELD_TOGGLE) {
        copy_string(dst, dst_size, string_is_true(field->value) ? "on" : "off");
        return;
    }
    if (field->type == ONETOOL_FIELD_CHOICE) {
        if (field->option_count > 0) {
            copy_string(dst, dst_size, field->options[field->choice_index].label);
            return;
        }
    }
    if (field->value[0] != '\0') {
        copy_string(dst, dst_size, field->value);
    } else {
        copy_string(dst, dst_size, field->placeholder);
    }
}

static void draw_form_screen(onetool_tui_state_t *state, int width, int height) {
    int left_width = width < 90 ? width / 3 : 28;
    int right_x = left_width + 1;
    int right_width = width - right_x;
    char preview[512];

    tui_draw_box(0, 0, left_width, height - 1, TUI_STYLE_PANEL, "Tool");
    tui_draw_box(right_x, 0, right_width, height - 1, TUI_STYLE_PANEL, "Arguments");

    tui_draw_text(2, 2, TUI_STYLE_ACCENT, state->active_tool->name);
    draw_wrapped_text(2, 4, left_width - 4, 7, TUI_STYLE_NORMAL, state->form.summary);
    tui_draw_textf(2, 12, TUI_STYLE_MUTED, "Theme: %s", g_themes[state->current_theme].name);
    tui_draw_text(2, 13, TUI_STYLE_MUTED, "Run path:");
    tui_draw_text(2, 14, TUI_STYLE_NORMAL, state->launch_dir);
    tui_draw_text(2, 16, TUI_STYLE_NORMAL, "Type to edit text fields.");
    tui_draw_text(2, 17, TUI_STYLE_NORMAL, "Enter runs the tool.");
    tui_draw_text(2, 18, TUI_STYLE_NORMAL, "Space or Left/Right changes options.");
    tui_draw_text(2, 19, TUI_STYLE_NORMAL, "Press N for settings, Esc to go back.");

    for (int i = 0; i < state->form.field_count; i++) {
        char value[256];
        int y = 2 + i * 2;
        format_field_value(&state->form.fields[i], value, sizeof(value));
        tui_draw_text(right_x + 2, y, i == state->selected_field ? TUI_STYLE_SELECTION : TUI_STYLE_ACCENT, state->form.fields[i].label);
        tui_draw_text(right_x + 2, y + 1, i == state->selected_field ? TUI_STYLE_SELECTION : (state->form.fields[i].value[0] == '\0' ? TUI_STYLE_MUTED : TUI_STYLE_NORMAL), value);
    }

    if (state->selected_field >= 0 && state->selected_field < state->form.field_count) {
        tui_draw_text(right_x + 2, height - 6, TUI_STYLE_MUTED, state->form.fields[state->selected_field].help);
    }
    build_preview_text(state->active_tool, &state->form, preview, sizeof(preview));
    tui_draw_text(right_x + 2, height - 4, TUI_STYLE_MUTED, "Preview:");
    tui_draw_text(right_x + 2, height - 3, TUI_STYLE_NORMAL, preview);
}

static void draw_settings_screen(onetool_tui_state_t *state, int width, int height) {
    int left_width = width < 90 ? width / 3 : 30;
    int right_x = left_width + 1;
    int right_width = width - right_x;

    tui_draw_box(0, 0, left_width, height - 1, TUI_STYLE_PANEL, "Settings");
    tui_draw_box(right_x, 0, right_width, height - 1, TUI_STYLE_PANEL, "Options");

    tui_draw_text(2, 2, TUI_STYLE_ACCENT, "Global settings");
    draw_wrapped_text(2, 4, left_width - 4, 7, TUI_STYLE_NORMAL, state->settings_form.summary);
    tui_draw_text(2, 12, TUI_STYLE_MUTED, "Active theme:");
    tui_draw_text(2, 13, TUI_STYLE_NORMAL, g_themes[state->current_theme].name);
    tui_draw_text(2, 15, TUI_STYLE_NORMAL, "Launch path changes where tools start.");
    tui_draw_text(2, 16, TUI_STYLE_NORMAL, "Theme edits apply live.");
    tui_draw_text(2, 17, TUI_STYLE_NORMAL, "Press N or Esc to go back.");

    for (int i = 0; i < state->settings_form.field_count; i++) {
        char value[256];
        int y = 2 + i * 2;
        format_field_value(&state->settings_form.fields[i], value, sizeof(value));
        tui_draw_text(right_x + 2, y, i == state->settings_selected_field ? TUI_STYLE_SELECTION : TUI_STYLE_ACCENT, state->settings_form.fields[i].label);
        tui_draw_text(right_x + 2, y + 1, i == state->settings_selected_field ? TUI_STYLE_SELECTION : (state->settings_form.fields[i].value[0] == '\0' ? TUI_STYLE_MUTED : TUI_STYLE_NORMAL), value);
    }

    if (state->settings_selected_field >= 0 && state->settings_selected_field < state->settings_form.field_count) {
        tui_draw_text(right_x + 2, height - 5, TUI_STYLE_MUTED, state->settings_form.fields[state->settings_selected_field].help);
    }
    tui_draw_text(right_x + 2, height - 3, TUI_STYLE_MUTED, "Preview updates live as you change colors.");
}

static void open_tool_form(onetool_tui_state_t *state, const struct onetool_tool *tool) {
    state->active_tool = tool;
    load_embedded_tool_form(&state->form, tool);
    state->selected_field = 0;
    state->screen = ONETOOL_SCREEN_FORM;
    if (!state->form.has_config) {
        set_status(state, "No tool config found. Using generic form plus extra args.");
    } else {
        snprintf(state->status, sizeof(state->status), "Editing %s", tool->name);
    }
}

static void close_tool_form(onetool_tui_state_t *state) {
    state->screen = ONETOOL_SCREEN_TOOLS;
    state->active_tool = NULL;
    memset(&state->form, 0, sizeof(state->form));
    set_status(state, "Returned to tool list.");
}

static void append_char(char *text, size_t text_size, int key) {
    size_t len = strlen(text);
    if (len + 1 < text_size && tui_key_is_printable(key)) {
        text[len] = (char)key;
        text[len + 1] = '\0';
    }
}

static void backspace_char(char *text) {
    size_t len = strlen(text);
    if (len > 0) {
        text[len - 1] = '\0';
    }
}

static void run_active_tool(onetool_tui_state_t *state, const char *onetool_argv0) {
    char argv_storage[TUI_MAX_ARGS][TUI_MAX_TOKEN];
    char *argv[TUI_MAX_ARGS];
    char error_text[160];
    char previous_dir[PATH_MAX];
    int argc;
    int rc;
    int changed_dir = 0;

    argc = build_tool_argv(state->active_tool, &state->form, argv_storage, argv, error_text, sizeof(error_text));
    if (argc < 0) {
        set_status(state, error_text);
        return;
    }

    tui_shutdown();
    printf("Running: onetool");
    for (int i = 1; i < argc; i++) {
        printf(" %s", argv[i]);
    }
    printf("\n\n");
    fflush(stdout);

    if (getcwd(previous_dir, sizeof(previous_dir)) == NULL) {
        copy_string(previous_dir, sizeof(previous_dir), ".");
    }
    if (state->launch_dir[0] != '\0' && strcmp(state->launch_dir, ".") != 0) {
        if (chdir(state->launch_dir) != 0) {
            printf("failed to switch to %s: %s\n", state->launch_dir, strerror(errno));
            printf("Press Enter to return to TUI...");
            fflush(stdout);
            for (;;) {
                int ch = getchar();
                if (ch == '\n' || ch == EOF) {
                    break;
                }
            }
            tui_init();
            tui_set_palette(&g_themes[state->current_theme].palette);
            snprintf(state->status, sizeof(state->status), "Cannot enter %s", state->launch_dir);
            return;
        }
        changed_dir = 1;
    }

    rc = onetool_run_tool(state->active_tool, argc, argv, onetool_argv0);
    if (changed_dir) {
        chdir(previous_dir);
    }
    printf("\nTool exited with code %d. Press Enter to return to TUI...", rc);
    fflush(stdout);
    for (;;) {
        int ch = getchar();
        if (ch == '\n' || ch == EOF) {
            break;
        }
    }

    tui_init();
    tui_set_palette(&g_themes[state->current_theme].palette);
    snprintf(state->status, sizeof(state->status), "%s finished with code %d", state->active_tool->name, rc);
}

static int handle_tool_screen_event(onetool_tui_state_t *state, const tui_event_t *event) {
    int total = onetool_total_tool_count();
    const struct onetool_tool *selected_tool = onetool_get_tool_by_index(state->selected_tool);

    if (event->kind != TUI_EVENT_KEY) {
        return 0;
    }

    int is_nav_key = (event->key == TUI_KEY_UP || event->key == TUI_KEY_DOWN || 
                      event->key == TUI_KEY_PAGE_UP || event->key == TUI_KEY_PAGE_DOWN || 
                      event->key == TUI_KEY_HOME || event->key == TUI_KEY_END);

    if (tool_is_taskmng(selected_tool) && !is_nav_key &&
        taskmng_handle_key(&state->taskmng_runtime, &state->taskmng_view, &state->taskmng_snapshot, event->key, state->status, sizeof(state->status))) {
        return 0;
    }

    switch (event->key) {
        case 'q':
        case TUI_KEY_ESCAPE:
            return 1;
        case TUI_KEY_UP:
            if (state->selected_tool > 0) {
                state->selected_tool--;
            }
            break;
        case TUI_KEY_DOWN:
            if (state->selected_tool + 1 < total) {
                state->selected_tool++;
            }
            break;
        case TUI_KEY_PAGE_UP:
            state->selected_tool -= 8;
            if (state->selected_tool < 0) {
                state->selected_tool = 0;
            }
            break;
        case TUI_KEY_PAGE_DOWN:
            state->selected_tool += 8;
            if (state->selected_tool >= total) {
                state->selected_tool = total - 1;
            }
            break;
        case TUI_KEY_HOME:
            state->selected_tool = 0;
            break;
        case TUI_KEY_END:
            state->selected_tool = total - 1;
            break;
        case 't':
            cycle_theme(state, 1);
            break;
        case 'T':
            cycle_theme(state, 1);
            break;
        case 'n':
        case 'N':
            open_settings(state, ONETOOL_SCREEN_TOOLS);
            break;
        case 'w':
        case 'W':
        case 's':
        case 'S':
        case 'k':
        case 'K':
        case 'f':
        case 'F':
            break;
        case TUI_KEY_ENTER:
            if (tool_is_taskmng(selected_tool)) {
                set_status(state, "Task manager is live in the right panel. Use W/S, k, t, f, g, h.");
            } else {
                open_tool_form(state, selected_tool);
            }
            break;
        default:
            break;
    }
    return 0;
}

static int handle_form_screen_event(onetool_tui_state_t *state, const tui_event_t *event, const char *onetool_argv0) {
    onetool_form_field_t *field;

    if (event->kind != TUI_EVENT_KEY || state->selected_field < 0 || state->selected_field >= state->form.field_count) {
        return 0;
    }
    field = &state->form.fields[state->selected_field];

    if (event->key == TUI_KEY_ENTER) {
        run_active_tool(state, onetool_argv0);
        return 0;
    }

    if (field->type == ONETOOL_FIELD_TEXT) {
        if (event->key == TUI_KEY_BACKSPACE) {
            backspace_char(field->value);
            return 0;
        }
        if (tui_key_is_printable(event->key)) {
            append_char(field->value, sizeof(field->value), event->key);
            return 0;
        }
    }

    switch (event->key) {
        case TUI_KEY_ESCAPE:
            close_tool_form(state);
            return 0;
        case TUI_KEY_UP:
            if (state->selected_field > 0) {
                state->selected_field--;
            }
            return 0;
        case TUI_KEY_DOWN:
        case TUI_KEY_TAB:
            if (state->selected_field + 1 < state->form.field_count) {
                state->selected_field++;
            } else {
                state->selected_field = 0;
            }
            return 0;
        case TUI_KEY_LEFT:
            if (field->type == ONETOOL_FIELD_CHOICE && field->option_count > 0) {
                field->choice_index--;
                if (field->choice_index < 0) {
                    field->choice_index = field->option_count - 1;
                }
                sync_choice_field(field);
            }
            return 0;
        case TUI_KEY_RIGHT:
            if (field->type == ONETOOL_FIELD_CHOICE && field->option_count > 0) {
                field->choice_index = (field->choice_index + 1) % field->option_count;
                sync_choice_field(field);
            }
            return 0;
        case ' ':
            if (field->type == ONETOOL_FIELD_TOGGLE) {
                copy_string(field->value, sizeof(field->value), string_is_true(field->value) ? "0" : "1");
            } else if (field->type == ONETOOL_FIELD_CHOICE && field->option_count > 0) {
                field->choice_index = (field->choice_index + 1) % field->option_count;
                sync_choice_field(field);
            }
            return 0;
        case 'r':
        case 'R':
            run_active_tool(state, onetool_argv0);
            return 0;
        case 't':
        case 'T':
            cycle_theme(state, 1);
            return 0;
        case 'n':
        case 'N':
            open_settings(state, ONETOOL_SCREEN_FORM);
            return 0;
        default:
            return 0;
    }
}

static int handle_settings_screen_event(onetool_tui_state_t *state, const tui_event_t *event) {
    onetool_form_field_t *field;

    if (event->kind != TUI_EVENT_KEY || state->settings_selected_field < 0 || state->settings_selected_field >= state->settings_form.field_count) {
        return 0;
    }
    field = &state->settings_form.fields[state->settings_selected_field];

    if (field->type == ONETOOL_FIELD_TEXT) {
        if (event->key == TUI_KEY_BACKSPACE) {
            backspace_char(field->value);
            if (strcmp(field->name, "launch_dir") == 0) {
                copy_string(state->launch_dir, sizeof(state->launch_dir), field->value[0] != '\0' ? field->value : ".");
            }
            return 0;
        }
        if (tui_key_is_printable(event->key)) {
            append_char(field->value, sizeof(field->value), event->key);
            if (strcmp(field->name, "launch_dir") == 0) {
                copy_string(state->launch_dir, sizeof(state->launch_dir), field->value);
            }
            return 0;
        }
    }

    switch (event->key) {
        case TUI_KEY_ESCAPE:
        case 'n':
        case 'N':
            close_settings(state);
            return 0;
        case TUI_KEY_UP:
            if (state->settings_selected_field > 0) {
                state->settings_selected_field--;
            }
            return 0;
        case TUI_KEY_DOWN:
        case TUI_KEY_TAB:
            if (state->settings_selected_field + 1 < state->settings_form.field_count) {
                state->settings_selected_field++;
            } else {
                state->settings_selected_field = 0;
            }
            return 0;
        case TUI_KEY_LEFT:
            if (field->type == ONETOOL_FIELD_CHOICE && field->option_count > 0) {
                field->choice_index--;
                if (field->choice_index < 0) {
                    field->choice_index = field->option_count - 1;
                }
                sync_choice_field(field);
                if (strcmp(field->name, "theme_preset") == 0) {
                    apply_theme_preset_from_settings(state);
                } else {
                    ensure_custom_theme(state);
                    sync_custom_palette_from_settings(state);
                }
            }
            return 0;
        case TUI_KEY_RIGHT:
        case TUI_KEY_ENTER:
        case ' ':
            if (field->type == ONETOOL_FIELD_CHOICE && field->option_count > 0) {
                field->choice_index = (field->choice_index + 1) % field->option_count;
                sync_choice_field(field);
                if (strcmp(field->name, "theme_preset") == 0) {
                    apply_theme_preset_from_settings(state);
                } else {
                    ensure_custom_theme(state);
                    sync_custom_palette_from_settings(state);
                }
            }
            return 0;
        default:
            return 0;
    }
}

int tui_main(const char *onetool_argv0) {
    onetool_tui_state_t state;
    tui_event_t event;
    int width;
    int height;

    memset(&state, 0, sizeof(state));
    init_embedded_themes();
    taskmng_runtime_init(&state.taskmng_runtime);
    taskmng_view_init(&state.taskmng_view);

    if (tui_init() != 0) {
        fprintf(stderr, "failed to initialize TUI\n");
        return 1;
    }

    tui_set_palette(&g_themes[0].palette);
    state.current_theme = 0;
    state.screen = ONETOOL_SCREEN_TOOLS;
    if (getcwd(state.launch_dir, sizeof(state.launch_dir)) == NULL) {
        copy_string(state.launch_dir, sizeof(state.launch_dir), ".");
    }
    set_status(&state, "Use arrows to select a tool. Press T for theme. N for settings");

    for (;;) {
        tui_get_size(&width, &height);
        tui_begin_frame();
        tui_clear(TUI_STYLE_NORMAL);

        if (state.screen == ONETOOL_SCREEN_TOOLS) {
            draw_tool_screen(&state, width, height);
        } else if (state.screen == ONETOOL_SCREEN_FORM) {
            draw_form_screen(&state, width, height);
        } else {
            draw_settings_screen(&state, width, height);
        }
        tui_draw_status_line(TUI_STYLE_PANEL, state.status);
        tui_end_frame();

        if (!tui_poll_event(&event, 250)) {
            continue;
        }
        if (state.screen == ONETOOL_SCREEN_TOOLS) {
            if (handle_tool_screen_event(&state, &event)) {
                break;
            }
        } else if (state.screen == ONETOOL_SCREEN_FORM) {
            handle_form_screen_event(&state, &event, onetool_argv0);
        } else {
            handle_settings_screen_event(&state, &event);
        }
    }

    tui_shutdown();
    return 0;
}
