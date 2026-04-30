#ifndef ONETOOL_CONFIG_TOOL_REGISTRY_H
#define ONETOOL_CONFIG_TOOL_REGISTRY_H

typedef int (*onetool_entry_fn)(int argc, char *argv[]);

enum onetool_argv0_mode {
    ONETOOL_ARGV0_TOOL_NAME = 0,
    ONETOOL_ARGV0_BINARY_PATH = 1,
};

struct onetool_tool {
    const char *name;
    onetool_entry_fn entry;
    const char *description;
    enum onetool_argv0_mode argv0_mode;
};

extern const struct onetool_tool onetool_builtin_tools[];
extern const int onetool_builtin_tool_count;
extern const struct onetool_tool *onetool_extra_tools;
extern const int onetool_extra_tool_count;
extern const char onetool_version[];

int onetool_total_tool_count(void);
const struct onetool_tool *onetool_get_tool_by_index(int index);
const struct onetool_tool *onetool_find_tool(const char *name);
int onetool_run_tool(const struct onetool_tool *tool, int argc, char *argv[], const char *onetool_argv0);
void onetool_show_help(void);

#endif
